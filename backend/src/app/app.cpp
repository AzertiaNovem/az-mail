// Owner: WP-A
// Process wiring for `azmail serve` (app.hpp has the ordered step list; DESIGN §3, A7, Addendum A).
#include "app/app.hpp"

#include "api/routes.hpp"
#include "app/config_loader.hpp"
#include "app/support.hpp"
#include "core/blob_store.hpp"
#include "core/log.hpp"
#include "core/signed_url.hpp"
#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "http/throttle.hpp"
#include "jobs/handlers.hpp"
#include "jobs/jobs.hpp"
#include "net/http_client.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "resend/rate_limiter.hpp"
#include "services.hpp"
#include "storage/r2_blob_store.hpp"
#include "ws/hub.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <csignal>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace azm::app {

namespace asio = boost::asio;
namespace fs = std::filesystem;

struct App::Impl {
  Config cfg;

  // Declaration order == construction order; the destructor tears down in reverse, so every
  // object outlives its users (HttpClient outlives the R2 store and the Resend client, Services
  // outlives the Runner, the io_context dies before the pools and everything it references).
  std::unique_ptr<db::Pool> pool;
  std::unique_ptr<net::HttpClient> http;
  std::unique_ptr<BlobStore> local_store;
  std::unique_ptr<BlobStore> r2_store;
  std::unique_ptr<SignedUrls> urls;
  std::unique_ptr<resend::RateLimiter> limiter;
  std::unique_ptr<resend::Client> resend;
  std::unique_ptr<ws::Hub> hub;
  std::unique_ptr<http::LoginThrottle> throttle;
  std::unique_ptr<Services> svc;
  std::unique_ptr<jobs::Runner> runner;
  std::unique_ptr<http::Router> router;
  std::unique_ptr<asio::thread_pool> db_workers;
  std::unique_ptr<asio::thread_pool> net_workers;
  std::unique_ptr<asio::thread_pool> files_workers;
  std::unique_ptr<asio::io_context> ioc;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::unique_ptr<http::Server> server;
  std::vector<std::thread> io_threads;

  std::mutex mu;  // start/stop
  bool started = false;
  bool stopped = false;

  explicit Impl(Config c) : cfg(std::move(c)) {}
  ~Impl() { shutdown(); }

  void build();
  void shutdown() noexcept;
};

void App::Impl::build() {
  // ---- 0. configuration ---------------------------------------------------------------------
  if (auto problems = validate_config(cfg, ConfigPurpose::Serve); !problems.empty()) {
    std::string msg = "invalid configuration / 配置无效:";
    for (const auto& p : problems) msg += "\n  " + p;
    throw std::runtime_error(msg);
  }
  if (auto lvl = log::parse_level(cfg.log_level)) log::set_level(*lvl);
  for (const auto& w : config_warnings(cfg)) log::warn(w);

  std::error_code ec;
  fs::create_directories(cfg.data_dir, ec);
  if (ec) throw std::runtime_error("cannot create AZMAIL_DATA_DIR " + cfg.data_dir + ": " + ec.message());
  if (auto parent = fs::path(cfg.db_path).parent_path(); !parent.empty()) {
    fs::create_directories(parent, ec);
    if (ec) throw std::runtime_error("cannot create the database directory " + parent.string() + ": " + ec.message());
  }

  // ---- 1. database ------------------------------------------------------------------------------
  pool = std::make_unique<db::Pool>(cfg.db_path, static_cast<std::size_t>(cfg.db_pool_size));
  {
    auto lease = pool->acquire();
    db::check_capabilities(*lease);
    const int v = db::migrate(*lease);
    log::info("database ready", {{"path", cfg.db_path}, {"schema_version", v}});
  }

  // ---- 2. outbound HTTP ---------------------------------------------------------------------------
  http = std::make_unique<net::HttpClient>(net::client_options_from(cfg));

  // ---- 3. R2 probe (may switch this App's delivery mode to proxy) ---------------------------------
  const bool use_r2 = r2_configured(cfg);
  if (use_r2) {
    const bool check_presign = cfg.files_delivery == FilesDelivery::Redirect;
    const auto report = storage::probe_r2(cfg, *http, /*write_test=*/false, check_presign);
    if (!report.ok()) {
      if (cfg.blob_backend == BlobBackend::R2)
        throw std::runtime_error("R2 bucket check failed / R2 存储桶检查失败: " + report.detail);
      log::warn("R2 is configured but not reachable; blobs stored in R2 will fail until it is",
                {{"detail", report.detail}});
    }
    if (check_presign && report.presign_overrides_ok != true) {
      log::warn("R2 presigned URLs do not honour response-content-* overrides; using proxy delivery",
                {{"detail", report.detail}});
      cfg.files_delivery = FilesDelivery::Proxy;
    }
  }

  // ---- 4. blob stores ------------------------------------------------------------------------------
  local_store = make_local_blob_store(cfg.data_dir);
  if (use_r2) r2_store = storage::make_r2_blob_store(cfg, *http);
  BlobStore* primary = cfg.blob_backend == BlobBackend::R2 ? r2_store.get() : local_store.get();
  BlobStore* secondary = cfg.blob_backend == BlobBackend::R2 ? local_store.get() : r2_store.get();
  {
    std::size_t swept = sweep_tmp_dir(primary->tmp_dir(), std::chrono::hours(24));
    if (secondary != nullptr && secondary->tmp_dir() != primary->tmp_dir())
      swept += sweep_tmp_dir(secondary->tmp_dir(), std::chrono::hours(24));
    if (swept > 0) log::info("removed stale staging files", {{"count", swept}});
  }

  // ---- 5. services ----------------------------------------------------------------------------------
  limiter = std::make_unique<resend::RateLimiter>(
      resend::RateLimiter::Options{cfg.resend_rate_rps, std::max(1.0, cfg.resend_rate_rps), 2.0});
  resend = std::make_unique<resend::Client>(cfg, *http, *limiter);
  urls = std::make_unique<SignedUrls>(cfg.server_secret, cfg.public_api_base_url, cfg.signed_url_ttl_sec * 1000);
  hub = std::make_unique<ws::Hub>(cfg.ws_max_sessions_per_user);
  throttle = std::make_unique<http::LoginThrottle>(cfg);
  svc = std::make_unique<Services>(Services{cfg, *pool, *primary, *urls, *hub, system_clock()});
  svc->resend = resend.get();
  svc->http = http.get();
  svc->rate_limiter = limiter.get();
  svc->login_throttle = throttle.get();
  svc->secondary_blobs = secondary;

  db_workers = std::make_unique<asio::thread_pool>(static_cast<std::size_t>(cfg.db_threads));
  net_workers = std::make_unique<asio::thread_pool>(static_cast<std::size_t>(cfg.net_threads));
  files_workers = std::make_unique<asio::thread_pool>(static_cast<std::size_t>(cfg.files_threads));
  svc->db_workers = db_workers.get();
  svc->net_workers = net_workers.get();
  svc->files_workers = files_workers.get();

  // ---- 6. jobs + routes -------------------------------------------------------------------------------
  runner = std::make_unique<jobs::Runner>(*pool, *svc, jobs::runner_config_from(cfg));
  jobs::register_outbound_jobs(*runner);
  jobs::register_inbound_jobs(*runner);
  jobs::register_maintenance_jobs(*runner);
  svc->runner = runner.get();
  // Events are published after COMMIT through the Hub; enqueues wake the runner.
  jobs::Runner* r = runner.get();
  pool->set_hooks(db::TxHooks{hub.get(), [r] { r->wake(); }});

  router = std::make_unique<http::Router>();
  api::register_routes(*router, cfg);

  // ---- 7. io + server ---------------------------------------------------------------------------------
  ioc = std::make_unique<asio::io_context>(std::max(cfg.io_threads, 1));
  work.emplace(asio::make_work_guard(*ioc));
  server = std::make_unique<http::Server>(
      *ioc, http::ServerDeps{cfg, *router, *svc, *db_workers, *net_workers, *hub, files_workers.get()});

  // ---- 8. go ---------------------------------------------------------------------------------------------
  server->start();
  try {
    const int purged = pool->write([&](db::Tx& tx) { return repo::purge_expired_sessions(tx, svc->now_ms()); });
    if (purged > 0) log::info("purged expired sessions", {{"count", purged}});
  } catch (const std::exception& e) {
    log::warn("startup session purge failed", {{"error", e.what()}});
  }
  runner->start();
  for (int i = 0; i < std::max(cfg.io_threads, 1); ++i)
    io_threads.emplace_back([this] {
      // A handler that throws must not take an io thread down: log and keep running.
      for (;;) {
        try {
          ioc->run();
          return;
        } catch (const std::exception& e) {
          log::error("exception escaped an io handler", {{"error", e.what()}});
        } catch (...) {
          log::error("unknown exception escaped an io handler");
        }
      }
    });
  log::info("azmail listening",
            {{"address", cfg.listen_address},
             {"port", server->port()},
             {"version", AZMAIL_VERSION},
             {"blob_backend", std::string(enum_name(cfg.blob_backend))},
             {"files_delivery", std::string(enum_name(cfg.files_delivery))}});
}

void App::Impl::shutdown() noexcept {
  try {
    // 1. Stop accepting; idle keep-alive connections close, busy ones finish their request.
    if (server) server->stop();
    // 2. WebSockets get 1001 going_away.
    if (hub) hub->close_all();
    // 3. Let in-flight requests and WS close handshakes finish (bounded).
    if (server && ioc && !io_threads.empty()) {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(std::clamp(cfg.shutdown_grace_sec, 1, 10));
      while (server->connections() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (server->connections() > 0)
        log::warn("shutdown: closing remaining connections", {{"count", server->connections()}});
    }
    // 4. Jobs (≤ cfg.shutdown_grace_sec; running jobs keep their lease and are recovered later).
    if (runner) runner->stop();
    // 5. Blocking pools: wait for queued handler work (their completions go to the io_context).
    if (db_workers) db_workers->join();
    if (net_workers) net_workers->join();
    if (files_workers) files_workers->join();
    if (pool) pool->set_hooks({});
    // 6. io_context.
    work.reset();
    if (ioc) ioc->stop();
    for (auto& t : io_threads)
      if (t.joinable()) t.join();
    io_threads.clear();
    // Destroy the server and then the io_context (remaining coroutine frames) while every object
    // they reference is still alive.
    server.reset();
    ioc.reset();
  } catch (const std::exception& e) {
    log::error("error during shutdown", {{"error", e.what()}});
  }
}

App::App(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
App::~App() { stop(); }

void App::start() {
  std::lock_guard lk(impl_->mu);
  if (impl_->started) throw std::logic_error("App::start called twice");
  impl_->started = true;
  try {
    impl_->build();
  } catch (const std::exception& e) {
    impl_->shutdown();
    impl_->stopped = true;
    throw std::runtime_error(std::string("startup failed / 启动失败: ") + e.what());
  }
}

void App::stop() {
  std::lock_guard lk(impl_->mu);
  if (!impl_->started || impl_->stopped) return;
  impl_->stopped = true;
  log::info("azmail shutting down");
  impl_->shutdown();
  log::info("azmail stopped");
}

int App::run() {
  std::signal(SIGPIPE, SIG_IGN);  // writes to closed sockets must fail with EPIPE, not kill us
  start();
  std::promise<int> done;
  auto finished = done.get_future();
  auto signals = std::make_unique<asio::signal_set>(*impl_->ioc, SIGINT, SIGTERM);
  signals->async_wait([&done](const boost::system::error_code& ec, int sig) {
    if (ec) return;
    log::info("signal received", {{"signal", sig}});
    done.set_value(0);
  });
  const int code = finished.get();
  signals.reset();
  stop();
  return code;
}

uint16_t App::port() const {
  if (!impl_->server) throw std::logic_error("App not started");
  return impl_->server->port();
}

Services& App::services() {
  if (!impl_->svc) throw std::logic_error("App not started");
  return *impl_->svc;
}

const Config& App::config() const { return impl_->cfg; }

}  // namespace azm::app
