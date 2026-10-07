// Owner: WP-A
// `azmail` command line (cli.hpp). Global options may appear anywhere; everything else belongs
// to the command. Exit codes: 0 ok, 1 runtime failure, 2 usage / configuration error.
#include "app/cli.hpp"

#include "api/dto.hpp"
#include "app/app.hpp"
#include "app/config_loader.hpp"
#include "app/support.hpp"
#include "core/address.hpp"
#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "mail/fts.hpp"
#include "net/http_client.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "resend/rate_limiter.hpp"
#include "storage/r2_blob_store.hpp"

#include <boost/json/serialize.hpp>
#include <boost/program_options.hpp>

#include <sqlite3.h>
#include <termios.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>

namespace azm::app {

namespace {

namespace po = boost::program_options;
namespace fs = std::filesystem;

constexpr int kOk = 0;
constexpr int kFail = 1;
constexpr int kUsage = 2;

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Io {
  std::istream& in;
  std::ostream& out;
  std::ostream& err;
};

constexpr std::string_view kUsageText = R"(azmail — AZ Mail server / AZ Mail 服务端

Usage / 用法:  azmail [global options] <command> [options]

Commands / 命令:
  serve                          run the HTTP/WebSocket server and job runner / 运行服务
  migrate                        apply pending database migrations / 执行数据库迁移
  create-user --email E [--name N] [--admin] [--create-domain] [--password-file F]
                                 create a user; the password is read from stdin (first line)
                                 / 创建用户（密码从标准输入第一行读取）
  reset-password --email E [--password-file F]
                                 set a new password and revoke all sessions / 重置密码并注销所有会话
  add-domain --name D            add a local mail domain / 添加本地域名
  backup --out FILE [--force]    online database backup (blobs are backed up separately)
                                 / 在线备份数据库（附件需另行备份）
  reindex                        rebuild the full-text search index / 重建全文索引
  doctor [--offline] [--r2]      check configuration, database, storage and Resend
                                 / 检查配置、数据库、存储和 Resend
  blobs-migrate --to r2|local [--dry-run]
                                 move stored blobs between storage backends / 在存储后端之间迁移附件
  version                        print the version / 显示版本

Global options / 全局选项 (anywhere on the command line):
  --env-file FILE                KEY=VALUE settings; real environment variables win / 从文件加载配置
  --set KEY=VALUE                override one setting (repeatable; wins over everything) / 覆盖配置项
  --data-dir DIR  --db-path FILE  --listen ADDR  --port N  --log-level LEVEL
                                 shortcuts for AZMAIL_DATA_DIR, AZMAIL_DB_PATH, AZMAIL_LISTEN_ADDRESS,
                                 AZMAIL_PORT, AZMAIL_LOG_LEVEL
  -h, --help                     this help; `azmail <command> --help` for a command / 显示帮助
  --version                      print the version

Exit codes / 退出码: 0 ok, 1 failure / 失败, 2 usage or configuration error / 用法或配置错误
)";

constexpr int kStyle = po::command_line_style::default_style & ~po::command_line_style::allow_guessing;

struct Invocation {
  std::optional<std::string> env_file;
  EnvMap overrides;
  std::string command;
  std::vector<std::string> args;
  bool help = false;
  bool version = false;
};

Invocation parse_invocation(const std::vector<std::string>& argv) {
  po::options_description g;
  g.add_options()("help,h", "")("version", "")("env-file", po::value<std::string>(), "")(
      "set", po::value<std::vector<std::string>>()->composing(), "")("data-dir", po::value<std::string>(), "")(
      "db-path", po::value<std::string>(), "")("listen", po::value<std::string>(), "")(
      "port", po::value<std::string>(), "")("log-level", po::value<std::string>(), "")(
      "command", po::value<std::string>(), "")("args", po::value<std::vector<std::string>>(), "");
  po::positional_options_description pos;
  pos.add("command", 1).add("args", -1);
  auto parsed = po::command_line_parser(argv).options(g).positional(pos).style(kStyle).allow_unregistered().run();
  po::variables_map vm;
  po::store(parsed, vm);

  Invocation inv;
  inv.help = vm.count("help") > 0;
  inv.version = vm.count("version") > 0;
  if (vm.count("env-file")) inv.env_file = vm["env-file"].as<std::string>();
  const std::pair<const char*, const char*> shortcuts[] = {{"data-dir", "AZMAIL_DATA_DIR"},
                                                           {"db-path", "AZMAIL_DB_PATH"},
                                                           {"listen", "AZMAIL_LISTEN_ADDRESS"},
                                                           {"port", "AZMAIL_PORT"},
                                                           {"log-level", "AZMAIL_LOG_LEVEL"}};
  for (const auto& [opt, key] : shortcuts)
    if (vm.count(opt)) inv.overrides[key] = vm[opt].as<std::string>();
  if (vm.count("set")) {
    for (const auto& kv : vm["set"].as<std::vector<std::string>>()) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos || eq == 0) throw UsageError("--set expects KEY=VALUE, got '" + kv + "'");
      inv.overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
  }
  // The command and its own options/arguments, in their original order.
  auto rest = po::collect_unrecognized(parsed.options, po::include_positional);
  if (!rest.empty()) {
    inv.command = rest.front();
    inv.args.assign(rest.begin() + 1, rest.end());
  }
  return inv;
}

// Parses a command's options; throws po::error for unknown/invalid ones.
po::variables_map parse_command(const std::vector<std::string>& args, const po::options_description& desc) {
  po::variables_map vm;
  po::store(po::command_line_parser(args).options(desc).style(kStyle).run(), vm);
  po::notify(vm);
  return vm;
}

std::string join_lines(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += "\n  " + x;
  return s;
}

ConfigLoad load_raw(const Invocation& inv, Io& io) {
  EnvMap file;
  if (inv.env_file) {
    try {
      file = read_env_file(*inv.env_file);
    } catch (const std::exception& e) {
      throw UsageError(e.what());
    }
  }
  auto loaded = config_from_env(merge_env(file, process_environment(), inv.overrides));
  for (const auto& w : loaded.warnings) io.err << "warning: " << w << "\n";
  if (auto lvl = log::parse_level(loaded.cfg.log_level)) log::set_level(*lvl);
  return loaded;
}

Config load_valid(const Invocation& inv, Io& io, ConfigPurpose purpose) {
  auto loaded = load_raw(inv, io);
  // Unparseable values and semantic problems are reported together, in one pass.
  auto problems = std::move(loaded.problems);
  for (auto& p : validate_config(loaded.cfg, purpose)) problems.push_back(std::move(p));
  if (!problems.empty()) throw UsageError("invalid configuration / 配置无效:" + join_lines(problems));
  return std::move(loaded.cfg);
}

std::unique_ptr<db::Pool> open_pool(const Config& cfg, bool must_exist) {
  std::error_code ec;
  if (must_exist && !fs::exists(cfg.db_path, ec))
    throw std::runtime_error("database not found: " + cfg.db_path +
                             " (run `azmail migrate` first) / 数据库不存在，请先运行 azmail migrate");
  if (auto parent = fs::path(cfg.db_path).parent_path(); !parent.empty()) fs::create_directories(parent, ec);
  auto pool = std::make_unique<db::Pool>(cfg.db_path, 2);
  {
    auto lease = pool->acquire();
    db::check_capabilities(*lease);
  }
  return pool;
}

void require_current_schema(db::Pool& pool) {
  auto lease = pool.acquire();
  const int v = db::current_version(*lease);
  if (v < db::latest_version())
    throw std::runtime_error("database schema version " + std::to_string(v) + " is older than " +
                             std::to_string(db::latest_version()) +
                             ": run `azmail migrate` first / 数据库需要先执行 azmail migrate");
}

// Password from --password-file (first line) or stdin (first line; no echo on a terminal).
std::string read_password(const po::variables_map& vm, Io& io) {
  std::string line;
  if (vm.count("password-file")) {
    const auto path = vm["password-file"].as<std::string>();
    std::ifstream f(path);
    if (!f) throw UsageError("cannot read password file " + path + " / 无法读取密码文件");
    std::getline(f, line);
  } else {
    const bool tty = &io.in == &std::cin && ::isatty(STDIN_FILENO) == 1;
    termios saved{};
    if (tty) {
      io.err << "Password / 密码: " << std::flush;
      if (::tcgetattr(STDIN_FILENO, &saved) == 0) {
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
      }
    }
    std::getline(io.in, line);
    if (tty) {
      ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
      io.err << "\n";
    }
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.empty()) throw UsageError("a password is required on stdin or via --password-file / 需要提供密码");
  return line;
}

std::string required_email(const po::variables_map& vm) {
  if (!vm.count("email")) throw UsageError("--email is required / 需要 --email");
  const std::string email = normalize_email(vm["email"].as<std::string>());
  if (!is_valid_email(email)) throw UsageError("invalid email address: " + email + " / 邮箱地址无效");
  return email;
}

bool wants_help(const po::variables_map& vm, const po::options_description& desc, std::string_view usage, Io& io) {
  if (!vm.count("help")) return false;
  io.out << "Usage / 用法: azmail " << usage << "\n" << desc;
  return true;
}

// ---- commands ----------------------------------------------------------------------------------

int cmd_serve(const Invocation& inv, Io& io) {
  po::options_description desc("serve options");
  desc.add_options()("help,h", "show help");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "serve", io)) return kOk;
  Config cfg = load_valid(inv, io, ConfigPurpose::Serve);
  App app(std::move(cfg));
  return app.run();
}

int cmd_migrate(const Invocation& inv, Io& io) {
  po::options_description desc("migrate options");
  desc.add_options()("help,h", "show help");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "migrate", io)) return kOk;
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  auto pool = open_pool(cfg, false);
  auto lease = pool->acquire();
  const int before = db::current_version(*lease);
  const int after = db::migrate(*lease);
  io.out << "database " << cfg.db_path << ": schema version " << after
         << (after == before ? " (up to date / 已是最新)" : " (migrated / 已迁移)") << "\n";
  return kOk;
}

int cmd_create_user(const Invocation& inv, Io& io) {
  po::options_description desc("create-user options");
  desc.add_options()("help,h", "show help")("email", po::value<std::string>(), "login / mailbox address")(
      "name", po::value<std::string>()->default_value(""), "display name")("admin", "grant admin rights")(
      "create-domain", "add the email's domain first when it is missing")(
      "password-file", po::value<std::string>(), "read the password from this file instead of stdin");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "create-user --email E [--name N] [--admin] [--create-domain]", io)) return kOk;
  const std::string email = required_email(vm);
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  const std::string password = read_password(vm, io);
  api::validate_new_password(password);
  const std::string hash = crypto::password_hash(password);  // outside any transaction (D1)

  auto pool = open_pool(cfg, true);
  require_current_schema(*pool);
  const bool admin = vm.count("admin") > 0;
  const bool create_domain = vm.count("create-domain") > 0;
  const int64_t now = now_ms();
  repo::NewUser nu;
  nu.email = email;
  nu.display_name = vm["name"].as<std::string>();
  nu.password_hash = hash;
  nu.is_admin = admin;
  nu.undo_send_seconds = cfg.default_undo_send_seconds;
  const auto user = pool->write([&](db::Tx& tx) {
    const std::string domain(domain_of(email));
    if (create_domain && !repo::find_domain(tx.conn(), domain)) repo::add_domain(tx, domain, now);
    auto u = repo::create_user(tx, nu, now);
    repo::audit(tx, std::nullopt, "user.create", "user:" + std::to_string(u.id),
                boost::json::object{{"via", "cli"}, {"is_admin", admin}}, "", now);
    return u;
  });
  io.out << "created user #" << user.id << " " << user.email << (user.is_admin ? " (admin)" : "") << "\n";
  return kOk;
}

int cmd_reset_password(const Invocation& inv, Io& io) {
  po::options_description desc("reset-password options");
  desc.add_options()("help,h", "show help")("email", po::value<std::string>(), "the user's login address")(
      "password-file", po::value<std::string>(), "read the password from this file instead of stdin");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "reset-password --email E", io)) return kOk;
  const std::string email = required_email(vm);
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  const std::string password = read_password(vm, io);
  api::validate_new_password(password);
  const std::string hash = crypto::password_hash(password);

  auto pool = open_pool(cfg, true);
  require_current_schema(*pool);
  const int64_t now = now_ms();
  const auto revoked = pool->write([&](db::Tx& tx) {
    auto u = repo::find_user_by_email(tx.conn(), email);
    if (!u) throw ApiError::not_found("not_found", "用户不存在: " + email);
    repo::UserPatch patch;
    patch.password_hash = hash;
    repo::update_user(tx, u->id, patch, now);
    auto ids = repo::revoke_all_sessions(tx, u->id);
    repo::audit(tx, std::nullopt, "password.reset", "user:" + std::to_string(u->id),
                boost::json::object{{"via", "cli"}, {"sessions_revoked", static_cast<int64_t>(ids.size())}}, "", now);
    return ids.size();
  });
  // A running server notices within its WS re-authentication period (5 min); HTTP requests
  // with the old tokens fail immediately.
  io.out << "password updated for " << email << "; " << revoked << " session(s) revoked / 已注销 " << revoked
         << " 个会话\n";
  return kOk;
}

int cmd_add_domain(const Invocation& inv, Io& io) {
  po::options_description desc("add-domain options");
  desc.add_options()("help,h", "show help")("name", po::value<std::string>(), "domain name, e.g. example.com");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "add-domain --name D", io)) return kOk;
  if (!vm.count("name")) throw UsageError("--name is required / 需要 --name");
  const std::string name = to_lower_ascii(trim(vm["name"].as<std::string>()));
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  auto pool = open_pool(cfg, true);
  require_current_schema(*pool);
  const int64_t now = now_ms();
  const auto d = pool->write([&](db::Tx& tx) {
    auto dom = repo::add_domain(tx, name, now);
    repo::audit(tx, std::nullopt, "domain.add", "domain:" + std::to_string(dom.id), boost::json::object{{"via", "cli"}},
                "", now);
    return dom;
  });
  io.out << "added domain #" << d.id << " " << d.name << "\n";
  return kOk;
}

int cmd_backup(const Invocation& inv, Io& io) {
  po::options_description desc("backup options");
  desc.add_options()("help,h", "show help")("out", po::value<std::string>(), "backup file to write")(
      "to", po::value<std::string>(), "alias of --out")("force", "overwrite an existing file");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "backup --out FILE [--force]", io)) return kOk;
  std::string out;
  if (vm.count("out")) out = vm["out"].as<std::string>();
  else if (vm.count("to")) out = vm["to"].as<std::string>();
  else throw UsageError("--out is required / 需要 --out");
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  {
    auto pool = open_pool(cfg, true);  // capability check + a clear "not found" message
  }
  const auto bytes = backup_database(cfg.db_path, out, vm.count("force") > 0);
  io.out << "backup written to " << out << " (" << bytes << " bytes). Back up "
         << (cfg.blob_backend == BlobBackend::R2 ? "the R2 bucket" : (fs::path(cfg.data_dir) / "blobs").string())
         << " separately / 附件请另行备份\n";
  return kOk;
}

int cmd_reindex(const Invocation& inv, Io& io) {
  po::options_description desc("reindex options");
  desc.add_options()("help,h", "show help")("batch", po::value<int>()->default_value(500), "messages per transaction");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "reindex", io)) return kOk;
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  auto pool = open_pool(cfg, true);
  require_current_schema(*pool);
  int64_t last_report = -1;
  const auto n = mail::fts_rebuild_all(*pool, std::max(vm["batch"].as<int>(), 1), [&](int64_t done, int64_t total) {
    if (done == total || done - last_report >= 5000) {
      io.err << "  " << done << "/" << total << "\n";
      last_report = done;
    }
  });
  io.out << "reindexed " << n << " message(s) / 已重建索引\n";
  return kOk;
}

// ---- doctor --------------------------------------------------------------------------------------

struct Report {
  Io& io;
  int failures = 0;
  int warnings = 0;
  void ok(const std::string& what) { io.out << "[ OK ] " << what << "\n"; }
  void warn(const std::string& what) {
    ++warnings;
    io.out << "[WARN] " << what << "\n";
  }
  void fail(const std::string& what) {
    ++failures;
    io.out << "[FAIL] " << what << "\n";
  }
};

std::string describe(const std::exception& e) {
  if (dynamic_cast<const NotImplemented*>(&e)) return std::string("not available in this build (") + e.what() + ")";
  if (const auto* api = dynamic_cast<const ApiError*>(&e)) return api->code + ": " + api->message;
  if (const auto* re = dynamic_cast<const resend::Error*>(&e))
    return "resend " + std::string(resend::to_string(re->kind)) + " (HTTP " + std::to_string(re->http_status) + ") " +
           re->name + " " + re->message;
  return e.what();
}

// Domain names in the `domains` table (read-only SQL so `doctor --offline` needs no repo).
std::vector<std::string> domain_names(db::Pool& pool) {
  return pool.read([](db::Conn& c) {
    std::vector<std::string> out;
    auto s = c.prepare("SELECT name FROM domains ORDER BY name");
    while (s.step()) out.push_back(s.text(0));
    return out;
  });
}

int cmd_doctor(const Invocation& inv, Io& io) {
  po::options_description desc("doctor options");
  desc.add_options()("help,h", "show help")("offline", "skip network checks (Resend, R2)")(
      "r2", "also verify that R2 presigned URLs honour response-content-* overrides (redirect mode)");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "doctor [--offline] [--r2]", io)) return kOk;
  const bool offline = vm.count("offline") > 0;
  const bool check_r2_presign = vm.count("r2") > 0;
  Report rep{io};
  io.out << "azmail " << AZMAIL_VERSION << " doctor\n";

  // 1. configuration
  ConfigLoad loaded = load_raw(inv, io);
  const Config& cfg = loaded.cfg;
  auto problems = loaded.problems;
  for (auto& p : validate_config(cfg, ConfigPurpose::Serve)) problems.push_back(std::move(p));
  if (problems.empty()) rep.ok("configuration / 配置");
  for (const auto& p : problems) rep.fail("configuration: " + p);
  for (const auto& w : config_warnings(cfg)) rep.warn(w);

  // 2. database
  std::unique_ptr<db::Pool> pool;
  try {
    pool = open_pool(cfg, true);
    auto lease = pool->acquire();
    const int v = db::current_version(*lease);
    const std::string where = " (SQLite " + std::string(sqlite3_libversion()) + ", " + cfg.db_path + ")";
    if (v < db::latest_version())
      rep.fail("database schema " + std::to_string(v) + "/" + std::to_string(db::latest_version()) +
               ": run `azmail migrate`" + where);
    else
      rep.ok("database schema " + std::to_string(v) + ", FTS5 trigram available" + where);
  } catch (const std::exception& e) {
    rep.fail("database: " + describe(e));
    pool.reset();
  }
  std::vector<std::string> domains_in_db;
  if (pool) {
    try {
      domains_in_db = domain_names(*pool);
      if (domains_in_db.empty())
        rep.warn("no mail domain yet: run `azmail add-domain --name <domain>` / 尚未添加域名");
      for (const auto& d : cfg.local_domains)
        if (std::find(domains_in_db.begin(), domains_in_db.end(), d) == domains_in_db.end())
          rep.warn("AZMAIL_LOCAL_DOMAINS lists " + d + " but it is not in the database (azmail add-domain) / 域名未添加");
    } catch (const std::exception& e) {
      rep.fail("database read: " + describe(e));
    }
  }

  // 3. local blob store (always: tmp staging lives there even with R2)
  try {
    auto local = make_local_blob_store(cfg.data_dir);
    const fs::path probe = make_staging_path(local->tmp_dir());
    {
      std::ofstream f(probe, std::ios::binary);
      f << "azmail doctor probe";
      if (!f) throw std::runtime_error("cannot write to " + local->tmp_dir().string());
    }
    std::error_code ec;
    fs::remove(probe, ec);
    rep.ok("local storage writable (" + cfg.data_dir + ")");
  } catch (const std::exception& e) {
    rep.fail("local storage: " + describe(e));
  }

  if (offline) {
    io.out << "(offline: Resend and R2 checks skipped / 已跳过网络检查)\n";
  } else {
    std::unique_ptr<net::HttpClient> http;
    try {
      http = std::make_unique<net::HttpClient>(net::client_options_from(cfg));
    } catch (const std::exception& e) {
      rep.fail("HTTP client: " + describe(e));
    }
    // 4. R2
    if (http && r2_configured(cfg)) {
      try {
        const bool presign = check_r2_presign || cfg.files_delivery == FilesDelivery::Redirect;
        const auto r = storage::probe_r2(cfg, *http, /*write_test=*/true, presign);
        if (!r.ok()) rep.fail("R2 bucket: " + r.detail);
        else if (!r.write_ok) rep.fail("R2 bucket reachable but the write test failed: " + r.detail);
        else rep.ok("R2 bucket " + cfg.r2_bucket + " reachable, write test passed");
        if (presign) {
          if (r.presign_overrides_ok == true) rep.ok("R2 presigned URLs honour response-content-* overrides");
          else if (cfg.files_delivery == FilesDelivery::Redirect)
            rep.warn("R2 presigned URLs do not honour response-content-* overrides: the server will use proxy "
                     "delivery / R2 预签名不支持响应头覆盖，将使用代理模式");
          else
            rep.fail("R2 presigned URLs do not honour response-content-* overrides (redirect mode unusable)");
        }
      } catch (const std::exception& e) {
        rep.fail("R2: " + describe(e));
      }
    } else if (check_r2_presign) {
      rep.fail("--r2: R2 is not configured / 未配置 R2");
    }
    // 5. Resend
    if (http && !cfg.resend_api_key.empty()) {
      try {
        resend::RateLimiter limiter({std::max(cfg.resend_rate_rps, 0.1), std::max(1.0, cfg.resend_rate_rps), 0.0});
        resend::Client client(cfg, *http, limiter);
        const auto domains = client.list_domains();
        rep.ok("Resend API reachable (" + std::to_string(domains.size()) + " domain(s))");
        for (const auto& name : domains_in_db) {
          auto it = std::find_if(domains.begin(), domains.end(),
                                 [&](const resend::DomainInfo& r) { return iequals(r.name, name); });
          if (it == domains.end()) rep.warn("domain " + name + " is not set up in Resend / 域名未在 Resend 配置");
          else if (it->status != "verified") rep.warn("domain " + name + " status in Resend: " + it->status);
          else rep.ok("domain " + name + " verified in Resend");
        }
      } catch (const std::exception& e) {
        rep.fail("Resend: " + describe(e));
      }
    } else if (cfg.resend_api_key.empty()) {
      rep.fail("Resend: RESEND_API_KEY is not set");
    }
  }

  io.out << "result: " << rep.failures << " failure(s), " << rep.warnings << " warning(s) / 结果：" << rep.failures
         << " 项失败，" << rep.warnings << " 项警告\n";
  return rep.failures == 0 ? kOk : kFail;
}

// ---- blobs-migrate ---------------------------------------------------------------------------------

int cmd_blobs_migrate(const Invocation& inv, Io& io) {
  po::options_description desc("blobs-migrate options");
  desc.add_options()("help,h", "show help")("to", po::value<std::string>(), "destination backend: r2 | local")(
      "dry-run", "only count what would be moved");
  auto vm = parse_command(inv.args, desc);
  if (wants_help(vm, desc, "blobs-migrate --to r2|local [--dry-run]", io)) return kOk;
  if (!vm.count("to")) throw UsageError("--to r2|local is required / 需要 --to r2|local");
  const std::string to = to_lower_ascii(vm["to"].as<std::string>());
  if (to != "r2" && to != "local") throw UsageError("--to must be r2 or local / --to 必须是 r2 或 local");
  Config cfg = load_valid(inv, io, ConfigPurpose::Offline);
  if (!r2_configured(cfg) || cfg.r2_access_key_id.empty() || cfg.r2_secret_access_key.empty() || cfg.r2_bucket.empty())
    throw UsageError(
        "R2 settings are required (R2_ACCOUNT_ID or R2_ENDPOINT, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, "
        "R2_BUCKET) / 需要完整的 R2 配置");
  auto pool = open_pool(cfg, true);
  require_current_schema(*pool);
  auto local = make_local_blob_store(cfg.data_dir);
  net::HttpClient http(net::client_options_from(cfg));
  auto r2 = storage::make_r2_blob_store(cfg, http);
  BlobStore& from = to == "r2" ? *local : *r2;
  BlobStore& dest = to == "r2" ? *r2 : *local;

  BlobMigrateOptions opts;
  opts.dry_run = vm.count("dry-run") > 0;
  int64_t seen = 0;
  opts.on_blob = [&](std::string_view sha, bool ok, std::string_view error) {
    ++seen;
    if (!ok) io.err << "  failed " << sha << ": " << error << "\n";
    else if (seen % 100 == 0) io.err << "  " << seen << " done\n";
  };
  const auto st = migrate_blobs(*pool, from, dest, opts);
  io.out << (opts.dry_run ? "dry run: " : "") << st.candidates << " blob(s) (" << st.bytes << " bytes) in "
         << from.kind() << "; migrated " << st.migrated << ", failed " << st.failed << "\n";
  if (!opts.dry_run && to == "r2" && cfg.blob_backend != BlobBackend::R2)
    io.out << "note: set AZMAIL_BLOB_BACKEND=r2 so new blobs are stored in R2 too / 请设置 AZMAIL_BLOB_BACKEND=r2\n";
  return st.failed == 0 ? kOk : kFail;
}

int dispatch(const Invocation& inv, Io& io) {
  if (inv.version || inv.command == "version") {
    io.out << "azmail " << AZMAIL_VERSION << "\n";
    return kOk;
  }
  if (inv.command.empty()) {
    io.out << kUsageText;
    return inv.help ? kOk : kUsage;
  }
  if (inv.help && inv.args.empty()) {
    // `azmail --help create-user` → that command's help.
    Invocation h = inv;
    h.args = {"--help"};
    h.help = false;
    return dispatch(h, io);
  }
  const std::string& c = inv.command;
  if (c == "serve") return cmd_serve(inv, io);
  if (c == "migrate") return cmd_migrate(inv, io);
  if (c == "create-user") return cmd_create_user(inv, io);
  if (c == "reset-password") return cmd_reset_password(inv, io);
  if (c == "add-domain") return cmd_add_domain(inv, io);
  if (c == "backup") return cmd_backup(inv, io);
  if (c == "reindex") return cmd_reindex(inv, io);
  if (c == "doctor") return cmd_doctor(inv, io);
  if (c == "blobs-migrate") return cmd_blobs_migrate(inv, io);
  if (c == "help") {
    io.out << kUsageText;
    return kOk;
  }
  throw UsageError("unknown command '" + c + "' (see azmail --help) / 未知命令");
}

}  // namespace

int run_cli(int argc, char** argv) { return run_cli(argc, argv, std::cin, std::cout, std::cerr); }

int run_cli(int argc, char** argv, std::istream& in, std::ostream& out, std::ostream& err) {
  Io io{in, out, err};
  try {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return dispatch(parse_invocation(args), io);
  } catch (const UsageError& e) {
    err << "error: " << e.what() << "\n";
    return kUsage;
  } catch (const po::error& e) {
    err << "error: " << e.what() << " (see azmail --help) / 参数错误\n";
    return kUsage;
  } catch (const ApiError& e) {
    err << "error: " << e.message << " [" << e.code << "]";
    if (!e.details.empty()) err << " " << boost::json::serialize(e.details);
    err << "\n";
    return kFail;
  } catch (const NotImplemented& e) {
    err << "error: not available in this build: " << e.what() << "\n";
    return kFail;
  } catch (const std::exception& e) {
    err << "error: " << e.what() << "\n";
    return kFail;
  }
}

}  // namespace azm::app
