// Owner: WP-A
//
// App: process wiring for `azmail serve` (DESIGN §3, A7, Addendum A).
// App keeps its OWN Config copy (impl) and may adjust it during start() (files_delivery
// fallback below); everything built afterwards — stores, Services, admin stats — sees it.
// start() builds, in order (members are destroyed in reverse, so every object outlives its
// users — notably net::HttpClient outlives the R2 store and the Resend client):
//  1. db::Pool (cfg.db_path, cfg.db_pool_size) + db::check_capabilities + db::migrate;
//  2. net::HttpClient (ClientOptions from cfg);
//  3. when R2 is configured (backend r2, or credentials present): storage::probe_r2(cfg, http,
//     write_test=false, check_presign_overrides = (files_delivery == Redirect)). Bucket not ok
//     with backend r2 → throw (backend local: warn, the secondary is still built and r2 blobs
//     fail per request with BlobError). Presign overrides not honoured → log a warning and set
//     the App's config files_delivery = Proxy BEFORE building the store and Services;
//  4. BlobStores: primary per cfg.blob_backend (make_local_blob_store(data_dir) /
//     storage::make_r2_blob_store(cfg, http)); secondary (Services::secondary_blobs) = the
//     local store when backend == r2 (always), the r2 store when backend == local and R2 is
//     configured — so every blobs.storage value resolves (Services::blobs_for);
//     then deletes regular files older than 24 h in each store's tmp_dir() (crash leftovers);
//  5. resend::RateLimiter + resend::Client; SignedUrls(cfg.server_secret,
//     cfg.public_api_base_url, cfg.signed_url_ttl_sec * 1000); ws::Hub (wired as
//     TxHooks::notifier); http::LoginThrottle; Services (db/net/files worker pointers set);
//  6. jobs::Runner (register_outbound/inbound/maintenance_jobs; TxHooks::wake_jobs →
//     Runner::wake); http::Router (api::register_routes(router, cfg));
//  7. the io_context threads, the "db" (cfg.db_threads), "net" (cfg.net_threads) and "files"
//     (cfg.files_threads) blocking pools and http::Server (ServerDeps::files_pool set);
//  8. starts listening and the runner.
#pragma once

#include "config.hpp"

#include <cstdint>
#include <memory>

namespace azm {
struct Services;
}

namespace azm::app {

class App {
 public:
  explicit App(Config cfg);
  ~App();  // calls stop()
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Wires everything and starts serving. Throws std::runtime_error with an actionable message
  // (no secrets) on any failure (invalid config, DB capability, R2 probe, bind).
  void start();
  // Graceful shutdown: close the acceptor, Hub::close_all (going_away), Runner::stop (≤
  // cfg.shutdown_grace_sec), join the blocking pools, stop the io_context. Idempotent.
  void stop();
  // start(); block until SIGINT/SIGTERM (asio::signal_set); stop(). Returns the exit code.
  int run();

  uint16_t port() const;      // bound HTTP port (after start)
  Services& services();       // valid after start (tests, CLI)
  const Config& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::app
