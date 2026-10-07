// Owner: WP-A
// Config loader: env / --env-file / CLI overrides → Config, plus semantic validation
// (config.hpp, app/config_loader.hpp).
#include "app/config_loader.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "http/cors.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/url/parse.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>

extern char** environ;

namespace azm {
namespace app {

namespace {

// ---- value parsers -----------------------------------------------------------------------------

template <class T>
bool parse_int(std::string_view s, T& out) {
  s = trim(s);
  if (s.empty()) return false;
  if (s.front() == '+') s.remove_prefix(1);
  T v{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return false;
  out = v;
  return true;
}

bool parse_size(std::string_view s, std::size_t& out) {
  s = trim(s);
  if (s.empty()) return false;
  std::uint64_t mult = 1;
  char last = s.back();
  if (last == 'B' || last == 'b') {  // "25MB", "25MiB" → treat as binary units
    s.remove_suffix(1);
    if (!s.empty() && (s.back() == 'i' || s.back() == 'I')) s.remove_suffix(1);
    if (s.empty()) return false;
    last = s.back();
  }
  switch (last) {
    case 'k': case 'K': mult = 1ull << 10; break;
    case 'm': case 'M': mult = 1ull << 20; break;
    case 'g': case 'G': mult = 1ull << 30; break;
    default: break;
  }
  if (mult != 1) s.remove_suffix(1);
  std::uint64_t v = 0;
  if (!parse_int(s, v)) return false;
  if (v > std::numeric_limits<std::size_t>::max() / mult) return false;
  out = static_cast<std::size_t>(v * mult);
  return true;
}

bool parse_bool(std::string_view s, bool& out) {
  const std::string v = to_lower_ascii(trim(s));
  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "no" || v == "off" || v.empty()) {
    out = false;
    return true;
  }
  return false;
}

bool parse_double(std::string_view s, double& out) {
  const std::string v(trim(s));
  if (v.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double d = std::strtod(v.c_str(), &end);
  if (errno != 0 || end != v.c_str() + v.size() || !std::isfinite(d)) return false;
  out = d;
  return true;
}

std::vector<std::string> parse_list(std::string_view s) {
  std::vector<std::string> out;
  for (const auto& part : split(s, ',')) {
    auto t = trim(part);
    if (!t.empty()) out.emplace_back(t);
  }
  return out;
}

std::string strip_trailing_slashes(std::string_view s) {
  s = trim(s);
  while (s.size() > 1 && s.back() == '/') s.remove_suffix(1);
  return std::string(s);
}

// ---- the key table -------------------------------------------------------------------------------

// Returns an error description ("" = ok). Never includes the value for secret keys.
using Setter = std::function<std::string(Config&, std::string_view)>;

struct Key {
  std::string_view name;
  Setter set;
  bool secret = false;
};

template <class T>
Setter integer(T Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    T out{};
    if (!parse_int(v, out)) return "not a valid integer / 不是有效的整数";
    c.*m = out;
    return {};
  };
}

Setter size_value(std::size_t Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    std::size_t out = 0;
    if (!parse_size(v, out)) return "not a valid size (bytes, or with K/M/G suffix) / 不是有效的大小";
    c.*m = out;
    return {};
  };
}

Setter boolean(bool Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    bool out = false;
    if (!parse_bool(v, out)) return "not a boolean (1/0/true/false) / 不是有效的布尔值";
    c.*m = out;
    return {};
  };
}

Setter text(std::string Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    c.*m = std::string(trim(v));
    return {};
  };
}

Setter url_value(std::string Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    c.*m = strip_trailing_slashes(v);
    return {};
  };
}

Setter list(std::vector<std::string> Config::*m) {
  return [m](Config& c, std::string_view v) -> std::string {
    c.*m = parse_list(v);
    return {};
  };
}

const std::vector<Key>& keys() {
  static const std::vector<Key> k = {
      // HTTP server
      {"AZMAIL_LISTEN_ADDRESS", text(&Config::listen_address)},
      {"AZMAIL_PORT", integer(&Config::listen_port)},
      {"AZMAIL_PUBLIC_API_URL", url_value(&Config::public_api_base_url)},
      {"AZMAIL_CORS_ORIGINS", list(&Config::cors_origins)},
      {"AZMAIL_TRUSTED_PROXIES", list(&Config::trusted_proxies)},
      {"AZMAIL_MAX_INFLIGHT", integer(&Config::inflight_cap)},
      {"AZMAIL_MAX_CONNECTIONS", integer(&Config::max_connections)},
      {"AZMAIL_HEADER_TIMEOUT_SEC", integer(&Config::header_timeout_sec)},
      {"AZMAIL_BODY_IDLE_TIMEOUT_SEC", integer(&Config::body_idle_timeout_sec)},
      {"AZMAIL_KEEPALIVE_TIMEOUT_SEC", integer(&Config::keepalive_timeout_sec)},
      // threads / pools
      {"AZMAIL_IO_THREADS", integer(&Config::io_threads)},
      {"AZMAIL_DB_THREADS", integer(&Config::db_threads)},
      {"AZMAIL_NET_THREADS", integer(&Config::net_threads)},
      {"AZMAIL_FILES_THREADS", integer(&Config::files_threads)},
      {"AZMAIL_DB_POOL_SIZE", integer(&Config::db_pool_size)},
      {"AZMAIL_JOBS_OUTBOUND_THREADS", integer(&Config::jobs_outbound_threads)},
      {"AZMAIL_JOBS_INBOUND_THREADS", integer(&Config::jobs_inbound_threads)},
      {"AZMAIL_JOBS_SYNC_THREADS", integer(&Config::jobs_sync_threads)},
      {"AZMAIL_JOBS_MAINTENANCE_THREADS", integer(&Config::jobs_maintenance_threads)},
      {"AZMAIL_SHUTDOWN_GRACE_SEC", integer(&Config::shutdown_grace_sec)},
      // storage
      {"AZMAIL_DATA_DIR", text(&Config::data_dir)},
      {"AZMAIL_DB_PATH", text(&Config::db_path)},
      // security
      {"AZMAIL_SECRET",
       [](Config& c, std::string_view v) -> std::string {
         auto s = decode_secret(v);
         if (!s) return "invalid hex:/base64: encoding / 编码无效";
         c.server_secret = std::move(*s);
         return {};
       },
       true},
      {"AZMAIL_SESSION_TTL_DAYS", integer(&Config::session_ttl_days)},
      {"AZMAIL_SESSION_TOUCH_SEC", integer(&Config::session_touch_interval_sec)},
      {"AZMAIL_LOGIN_MAX_PER_EMAIL", integer(&Config::login_max_per_email)},
      {"AZMAIL_LOGIN_MAX_PER_IP", integer(&Config::login_max_per_ip)},
      {"AZMAIL_LOGIN_WINDOW_SEC", integer(&Config::login_window_sec)},
      {"AZMAIL_SCRYPT_CONCURRENCY", integer(&Config::scrypt_concurrency)},
      {"AZMAIL_SIGNED_URL_TTL_SEC", integer(&Config::signed_url_ttl_sec)},
      // mail domain
      {"AZMAIL_LOCAL_DOMAINS",
       [](Config& c, std::string_view v) -> std::string {
         c.local_domains.clear();
         for (auto& d : parse_list(v)) c.local_domains.push_back(to_lower_ascii(d));
         return {};
       }},
      {"AZMAIL_UNDO_SEND_SECONDS", integer(&Config::default_undo_send_seconds)},
      {"AZMAIL_SCHEDULE_MODE",
       [](Config& c, std::string_view v) -> std::string {
         const auto s = to_lower_ascii(trim(v));
         if (s == "resend") c.schedule_mode = ScheduleMode::Resend;
         else if (s == "local") c.schedule_mode = ScheduleMode::Local;
         else return "must be 'resend' or 'local' / 必须是 resend 或 local";
         return {};
       }},
      {"AZMAIL_SCHEDULE_MIN_LEAD_SEC", integer(&Config::schedule_min_lead_sec)},
      {"AZMAIL_SCHEDULE_MAX_DAYS", integer(&Config::schedule_max_days)},
      {"AZMAIL_MAX_RECIPIENTS", integer(&Config::max_recipients_per_field)},
      {"AZMAIL_UNROUTABLE_TO_ADMINS", boolean(&Config::unroutable_to_admins)},
      // size limits
      {"AZMAIL_MAX_HEADER_BYTES", size_value(&Config::max_header_bytes)},
      {"AZMAIL_JSON_BODY_LIMIT", size_value(&Config::json_body_limit)},
      {"AZMAIL_DRAFT_BODY_LIMIT", size_value(&Config::draft_body_limit)},
      {"AZMAIL_UPLOAD_LIMIT", size_value(&Config::upload_body_limit)},
      {"AZMAIL_WEBHOOK_BODY_LIMIT", size_value(&Config::webhook_body_limit)},
      {"AZMAIL_MAX_MESSAGE_ATTACHMENTS", size_value(&Config::max_message_attachment_bytes)},
      {"AZMAIL_INBOUND_ATTACHMENT_LIMIT", size_value(&Config::inbound_attachment_limit)},
      {"AZMAIL_INBOUND_RAW_LIMIT", size_value(&Config::inbound_raw_limit)},
      // retention / maintenance
      {"AZMAIL_TRASH_PURGE_DAYS", integer(&Config::trash_purge_days)},
      {"AZMAIL_SPAM_PURGE_DAYS", integer(&Config::spam_purge_days)},
      {"AZMAIL_UPLOAD_TTL_HOURS", integer(&Config::unattached_upload_ttl_hours)},
      {"AZMAIL_BLOB_GC_GRACE_HOURS", integer(&Config::blob_gc_grace_hours)},
      {"AZMAIL_JOBS_RETENTION_DAYS", integer(&Config::jobs_done_retention_days)},
      {"AZMAIL_WEBHOOK_RETENTION_DAYS", integer(&Config::webhook_events_retention_days)},
      {"AZMAIL_POLL_INTERVAL_SEC", integer(&Config::poll_interval_sec)},
      {"AZMAIL_RECONCILE_INTERVAL_SEC", integer(&Config::reconcile_interval_sec)},
      // Resend
      {"RESEND_API_KEY", text(&Config::resend_api_key), true},
      {"RESEND_API_BASE", url_value(&Config::resend_api_base)},
      {"RESEND_WEBHOOK_SECRET", text(&Config::resend_webhook_secret), true},
      {"RESEND_USER_AGENT", text(&Config::resend_user_agent)},
      {"RESEND_RATE_RPS",
       [](Config& c, std::string_view v) -> std::string {
         double d = 0;
         if (!parse_double(v, d)) return "not a number / 不是有效的数字";
         c.resend_rate_rps = d;
         return {};
       }},
      {"RESEND_TIMEOUT_SEC", integer(&Config::resend_timeout_sec)},
      {"AZMAIL_WEBHOOK_TOLERANCE_SEC", integer(&Config::webhook_tolerance_sec)},
      // outbound HTTP client
      {"AZMAIL_ALLOW_INSECURE_HTTP", boolean(&Config::allow_insecure_http)},
      {"AZMAIL_CA_FILE", text(&Config::ca_file)},
      // blob storage
      {"AZMAIL_BLOB_BACKEND",
       [](Config& c, std::string_view v) -> std::string {
         const auto s = to_lower_ascii(trim(v));
         if (s == "local") c.blob_backend = BlobBackend::Local;
         else if (s == "r2") c.blob_backend = BlobBackend::R2;
         else return "must be 'local' or 'r2' / 必须是 local 或 r2";
         return {};
       }},
      {"R2_ACCOUNT_ID", text(&Config::r2_account_id)},
      {"R2_ACCESS_KEY_ID", text(&Config::r2_access_key_id), true},
      {"R2_SECRET_ACCESS_KEY", text(&Config::r2_secret_access_key), true},
      {"R2_BUCKET", text(&Config::r2_bucket)},
      {"R2_ENDPOINT", url_value(&Config::r2_endpoint)},
      {"R2_PREFIX", text(&Config::r2_prefix)},
      {"AZMAIL_FILES_DELIVERY",
       [](Config& c, std::string_view v) -> std::string {
         const auto s = to_lower_ascii(trim(v));
         if (s == "proxy") c.files_delivery = FilesDelivery::Proxy;
         else if (s == "redirect") c.files_delivery = FilesDelivery::Redirect;
         else return "must be 'proxy' or 'redirect' / 必须是 proxy 或 redirect";
         return {};
       }},
      {"R2_PRESIGN_TTL_SEC", integer(&Config::r2_presign_ttl_sec)},
      {"AZMAIL_FILE_CACHE_MB", integer(&Config::file_cache_mb)},
      // logging
      {"AZMAIL_LOG_LEVEL",
       [](Config& c, std::string_view v) -> std::string {
         const auto s = to_lower_ascii(trim(v));
         if (!log::parse_level(s)) return "must be trace|debug|info|warn|error|off / 日志级别无效";
         c.log_level = s;
         return {};
       }},
      // WebSocket
      {"AZMAIL_WS_AUTH_TIMEOUT_MS", integer(&Config::ws_auth_timeout_ms)},
      {"AZMAIL_WS_MAX_MESSAGE_BYTES", size_value(&Config::ws_max_message_bytes)},
      {"AZMAIL_WS_QUEUE_CAP", integer(&Config::ws_queue_cap)},
      {"AZMAIL_WS_MAX_PER_USER", integer(&Config::ws_max_sessions_per_user)},
  };
  return k;
}

const Key* find_key(std::string_view name) {
  for (const auto& k : keys())
    if (k.name == name) return &k;
  return nullptr;
}

bool is_managed_name(std::string_view name) {
  return name.starts_with("AZMAIL_") || name.starts_with("RESEND_") || name.starts_with("R2_");
}

// ---- validation helpers ------------------------------------------------------------------------

bool valid_http_url(std::string_view s, bool allow_path) {
  auto r = boost::urls::parse_absolute_uri(boost::core::string_view(s.data(), s.size()));
  if (!r) return false;
  const auto scheme = to_lower_ascii(std::string_view(r->scheme()));
  if (scheme != "http" && scheme != "https") return false;
  if (!r->has_authority() || r->encoded_host().empty() || r->has_userinfo()) return false;
  if (r->has_query() || r->has_fragment()) return false;
  if (!allow_path && !r->encoded_path().empty() && r->encoded_path() != "/") return false;
  return true;
}

bool is_https(std::string_view s) { return istarts_with(trim(s), "https://"); }

bool valid_ip_or_cidr(std::string_view spec) {
  spec = trim(spec);
  const auto slash = spec.find('/');
  const std::string addr_s(spec.substr(0, slash));
  boost::system::error_code ec;
  const auto addr = boost::asio::ip::make_address(addr_s, ec);
  if (ec) return false;
  if (slash == std::string_view::npos) return true;
  int bits = -1;
  const std::string_view b = spec.substr(slash + 1);
  auto [p, e] = std::from_chars(b.data(), b.data() + b.size(), bits);
  if (e != std::errc() || p != b.data() + b.size()) return false;
  return bits >= 0 && bits <= (addr.is_v4() ? 32 : 128);
}

}  // namespace

// ---- env files / environment -------------------------------------------------------------------

EnvMap parse_env_file(std::string_view text, std::vector<std::string>& errors) {
  EnvMap out;
  int line_no = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::string_view l = trim(line);
    if (l.empty() || l.front() == '#') continue;
    if (l.starts_with("export ") || l.starts_with("export\t")) l = trim(l.substr(7));
    const auto eq = l.find('=');
    if (eq == std::string_view::npos) {
      errors.push_back("line " + std::to_string(line_no) + ": expected KEY=VALUE / 应为 KEY=VALUE");
      continue;
    }
    const std::string_view key = trim(l.substr(0, eq));
    bool key_ok = !key.empty() && !(key.front() >= '0' && key.front() <= '9');
    for (char c : key)
      key_ok = key_ok && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    if (!key_ok) {
      errors.push_back("line " + std::to_string(line_no) + ": invalid variable name / 变量名无效");
      continue;
    }
    std::string_view raw = trim(l.substr(eq + 1));
    std::string value;
    if (!raw.empty() && (raw.front() == '"' || raw.front() == '\'')) {
      const char q = raw.front();
      bool closed = false;
      std::size_t i = 1;
      for (; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == q) {
          closed = true;
          break;
        }
        if (q == '"' && c == '\\' && i + 1 < raw.size()) {
          const char n = raw[++i];
          switch (n) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            default: value += n; break;  // \" \\ \$ …
          }
          continue;
        }
        value += c;
      }
      const std::string_view rest = closed ? trim(raw.substr(i + 1)) : std::string_view();
      if (!closed || (!rest.empty() && rest.front() != '#')) {
        errors.push_back("line " + std::to_string(line_no) + ": unterminated or malformed quoted value / 引号不匹配");
        continue;
      }
    } else {
      // Unquoted: an inline comment starts at whitespace followed by '#'.
      std::size_t cut = raw.size();
      for (std::size_t i = 1; i < raw.size(); ++i) {
        if (raw[i] == '#' && (raw[i - 1] == ' ' || raw[i - 1] == '\t')) {
          cut = i;
          break;
        }
      }
      value = std::string(trim(raw.substr(0, cut)));
    }
    out[std::string(key)] = std::move(value);
  }
  return out;
}

EnvMap read_env_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read env file " + path.string() + " / 无法读取环境变量文件");
  std::ostringstream ss;
  ss << in.rdbuf();
  std::vector<std::string> errors;
  EnvMap m = parse_env_file(ss.str(), errors);
  if (!errors.empty()) {
    std::string msg = "invalid env file " + path.string() + " / 环境变量文件有误:";
    for (const auto& e : errors) msg += "\n  " + e;
    throw std::runtime_error(msg);
  }
  return m;
}

EnvMap process_environment() {
  EnvMap out;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    const std::string_view kv(*e);
    const auto eq = kv.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view k = kv.substr(0, eq);
    if (is_managed_name(k)) out[std::string(k)] = std::string(kv.substr(eq + 1));
  }
  return out;
}

EnvMap merge_env(const EnvMap& file, const EnvMap& process, const EnvMap& overrides) {
  EnvMap out = file;
  for (const auto& [k, v] : process) out[k] = v;
  for (const auto& [k, v] : overrides) out[k] = v;
  return out;
}

std::optional<std::string> decode_secret(std::string_view value) {
  value = trim(value);
  if (istarts_with(value, "hex:")) return crypto::hex_decode(trim(value.substr(4)));
  if (istarts_with(value, "base64:")) return crypto::b64_decode(trim(value.substr(7)));
  return std::string(value);
}

ConfigLoad config_from_env(const EnvMap& env) {
  ConfigLoad r;
  for (const auto& [name, value] : env) {
    const Key* k = find_key(name);
    if (k == nullptr) {
      if (is_managed_name(name)) r.warnings.push_back(name + ": unknown setting, ignored / 未知配置项，已忽略");
      continue;
    }
    const std::string err = k->set(r.cfg, value);
    if (!err.empty()) {
      std::string msg = name + ": " + err;
      if (!k->secret) msg += " ('" + utf8_truncate(value, 64) + "')";
      r.problems.push_back(std::move(msg));
    }
  }
  // Derived defaults.
  if (env.count("AZMAIL_DATA_DIR") && !env.count("AZMAIL_DB_PATH"))
    r.cfg.db_path = (std::filesystem::path(r.cfg.data_dir) / "azmail.db").string();
  if (!env.count("AZMAIL_PUBLIC_API_URL") && (env.count("AZMAIL_PORT") || env.count("AZMAIL_LISTEN_ADDRESS"))) {
    std::string host = r.cfg.listen_address;
    if (host == "0.0.0.0" || host.empty()) host = "127.0.0.1";
    else if (host == "::") host = "[::1]";
    else if (host.find(':') != std::string::npos) host = "[" + host + "]";
    r.cfg.public_api_base_url = "http://" + host + ":" + std::to_string(r.cfg.listen_port);
  }
  return r;
}

// ---- validation ----------------------------------------------------------------------------------

std::vector<std::string> validate_config(const Config& c, ConfigPurpose purpose) {
  std::vector<std::string> p;
  auto need = [&](bool ok, std::string msg) {
    if (!ok) p.push_back(std::move(msg));
  };
  auto at_least = [&](auto v, auto lo, std::string_view key) {
    if (v < lo) p.push_back(std::string(key) + " must be >= " + std::to_string(lo) + " / 必须 >= " + std::to_string(lo));
  };

  // ---- always ------------------------------------------------------------------------------
  need(!trim(c.data_dir).empty(), "AZMAIL_DATA_DIR must not be empty / AZMAIL_DATA_DIR 不能为空");
  need(!trim(c.db_path).empty(), "AZMAIL_DB_PATH must not be empty / AZMAIL_DB_PATH 不能为空");
  need(log::parse_level(c.log_level).has_value(),
       "AZMAIL_LOG_LEVEL must be trace|debug|info|warn|error|off / 日志级别无效");
  at_least(c.db_pool_size, 1, "AZMAIL_DB_POOL_SIZE");
  need(c.default_undo_send_seconds == 0 || c.default_undo_send_seconds == 5 || c.default_undo_send_seconds == 10 ||
           c.default_undo_send_seconds == 20 || c.default_undo_send_seconds == 30,
       "AZMAIL_UNDO_SEND_SECONDS must be 0, 5, 10, 20 or 30 / 撤销发送时间必须是 0、5、10、20 或 30 秒");
  if (purpose == ConfigPurpose::Offline) return p;

  // ---- HTTP server -----------------------------------------------------------------------------
  {
    boost::system::error_code ec;
    (void)boost::asio::ip::make_address(c.listen_address, ec);
    need(!ec, "AZMAIL_LISTEN_ADDRESS must be an IP address (e.g. 127.0.0.1, 0.0.0.0, ::) / 监听地址必须是 IP 地址");
  }
  need(valid_http_url(c.public_api_base_url, true),
       "AZMAIL_PUBLIC_API_URL must be an http(s) URL without query, e.g. https://mail-api.example.com / "
       "AZMAIL_PUBLIC_API_URL 必须是 http(s) 地址");
  for (const auto& o : c.cors_origins) {
    std::string_view v = trim(o);
    while (!v.empty() && v.back() == '/') v.remove_suffix(1);
    need(http::normalize_origin(v).has_value(),
         "AZMAIL_CORS_ORIGINS entry '" + utf8_truncate(o, 100) +
             "' is not an origin like https://mail.example.com (no path, no wildcard) / 跨域来源格式无效");
  }
  for (const auto& t : c.trusted_proxies)
    need(valid_ip_or_cidr(t), "AZMAIL_TRUSTED_PROXIES entry '" + utf8_truncate(t, 100) +
                                  "' is not an IP address or CIDR block / 受信代理必须是 IP 或 CIDR");
  at_least(c.inflight_cap, std::size_t{1}, "AZMAIL_MAX_INFLIGHT");
  at_least(c.max_connections, std::size_t{1}, "AZMAIL_MAX_CONNECTIONS");
  at_least(c.header_timeout_sec, 1, "AZMAIL_HEADER_TIMEOUT_SEC");
  at_least(c.body_idle_timeout_sec, 1, "AZMAIL_BODY_IDLE_TIMEOUT_SEC");
  at_least(c.keepalive_timeout_sec, 1, "AZMAIL_KEEPALIVE_TIMEOUT_SEC");

  // ---- threads ---------------------------------------------------------------------------------
  at_least(c.io_threads, 1, "AZMAIL_IO_THREADS");
  at_least(c.db_threads, 1, "AZMAIL_DB_THREADS");
  at_least(c.net_threads, 1, "AZMAIL_NET_THREADS");
  at_least(c.files_threads, 1, "AZMAIL_FILES_THREADS");
  at_least(c.jobs_outbound_threads, 0, "AZMAIL_JOBS_OUTBOUND_THREADS");
  at_least(c.jobs_inbound_threads, 0, "AZMAIL_JOBS_INBOUND_THREADS");
  at_least(c.jobs_sync_threads, 0, "AZMAIL_JOBS_SYNC_THREADS");
  at_least(c.jobs_maintenance_threads, 0, "AZMAIL_JOBS_MAINTENANCE_THREADS");
  at_least(c.shutdown_grace_sec, 1, "AZMAIL_SHUTDOWN_GRACE_SEC");

  // ---- security ----------------------------------------------------------------------------------
  if (c.server_secret.empty())
    p.push_back("AZMAIL_SECRET is required (>= 32 random bytes, e.g. `openssl rand -hex 32`) / "
                "必须设置 AZMAIL_SECRET（至少 32 字节随机值）");
  else if (c.server_secret.size() < 32)
    p.push_back("AZMAIL_SECRET is too short: it needs >= 32 bytes / AZMAIL_SECRET 太短，至少需要 32 字节");
  at_least(c.session_ttl_days, 1, "AZMAIL_SESSION_TTL_DAYS");
  at_least(c.session_touch_interval_sec, 1, "AZMAIL_SESSION_TOUCH_SEC");
  at_least(c.login_max_per_email, 1, "AZMAIL_LOGIN_MAX_PER_EMAIL");
  at_least(c.login_max_per_ip, 1, "AZMAIL_LOGIN_MAX_PER_IP");
  at_least(c.login_window_sec, 1, "AZMAIL_LOGIN_WINDOW_SEC");
  at_least(c.scrypt_concurrency, 1, "AZMAIL_SCRYPT_CONCURRENCY");
  at_least(c.signed_url_ttl_sec, int64_t{1}, "AZMAIL_SIGNED_URL_TTL_SEC");

  // ---- mail ----------------------------------------------------------------------------------------
  at_least(c.schedule_min_lead_sec, 0, "AZMAIL_SCHEDULE_MIN_LEAD_SEC");
  need(c.schedule_max_days >= 1 && c.schedule_max_days <= 30,
       "AZMAIL_SCHEDULE_MAX_DAYS must be 1..30 (Resend limit) / 定时发送最多 30 天");
  need(c.max_recipients_per_field >= 1 && c.max_recipients_per_field <= 50,
       "AZMAIL_MAX_RECIPIENTS must be 1..50 (Resend limit) / 收件人上限必须在 1 到 50 之间");
  for (const auto& d : c.local_domains)
    need(d.find('.') != std::string::npos && d.find('@') == std::string::npos && d.find(' ') == std::string::npos,
         "AZMAIL_LOCAL_DOMAINS entry '" + utf8_truncate(d, 100) + "' is not a domain name / 本地域名格式无效");

  // ---- limits ---------------------------------------------------------------------------------------
  at_least(c.max_header_bytes, std::size_t{1024}, "AZMAIL_MAX_HEADER_BYTES");
  at_least(c.json_body_limit, std::size_t{1024}, "AZMAIL_JSON_BODY_LIMIT");
  at_least(c.draft_body_limit, std::size_t{1024}, "AZMAIL_DRAFT_BODY_LIMIT");
  at_least(c.upload_body_limit, std::size_t{1}, "AZMAIL_UPLOAD_LIMIT");
  at_least(c.webhook_body_limit, std::size_t{1024}, "AZMAIL_WEBHOOK_BODY_LIMIT");
  at_least(c.max_message_attachment_bytes, std::size_t{1}, "AZMAIL_MAX_MESSAGE_ATTACHMENTS");
  at_least(c.inbound_attachment_limit, std::size_t{1}, "AZMAIL_INBOUND_ATTACHMENT_LIMIT");
  at_least(c.inbound_raw_limit, std::size_t{1}, "AZMAIL_INBOUND_RAW_LIMIT");
  need(c.max_header_bytes <= (1u << 20), "AZMAIL_MAX_HEADER_BYTES must be <= 1M / 请求头上限不能超过 1M");

  // ---- retention -------------------------------------------------------------------------------------
  at_least(c.trash_purge_days, 1, "AZMAIL_TRASH_PURGE_DAYS");
  at_least(c.spam_purge_days, 1, "AZMAIL_SPAM_PURGE_DAYS");
  at_least(c.unattached_upload_ttl_hours, 1, "AZMAIL_UPLOAD_TTL_HOURS");
  at_least(c.blob_gc_grace_hours, 1, "AZMAIL_BLOB_GC_GRACE_HOURS");
  at_least(c.jobs_done_retention_days, 1, "AZMAIL_JOBS_RETENTION_DAYS");
  at_least(c.webhook_events_retention_days, 1, "AZMAIL_WEBHOOK_RETENTION_DAYS");
  at_least(c.poll_interval_sec, 5, "AZMAIL_POLL_INTERVAL_SEC");
  at_least(c.reconcile_interval_sec, 10, "AZMAIL_RECONCILE_INTERVAL_SEC");

  // ---- Resend ------------------------------------------------------------------------------------------
  need(!c.resend_api_key.empty(), "RESEND_API_KEY is required / 必须设置 RESEND_API_KEY");
  need(valid_http_url(c.resend_api_base, true), "RESEND_API_BASE must be an http(s) URL / RESEND_API_BASE 必须是 http(s) 地址");
  need(is_https(c.resend_api_base) || c.allow_insecure_http,
       "RESEND_API_BASE uses http:// but AZMAIL_ALLOW_INSECURE_HTTP is not set (mock only) / "
       "RESEND_API_BASE 使用 http:// 时必须设置 AZMAIL_ALLOW_INSECURE_HTTP=1（仅限模拟服务）");
  need(c.resend_webhook_secret.empty() || c.resend_webhook_secret.starts_with("whsec_"),
       "RESEND_WEBHOOK_SECRET must start with 'whsec_' / RESEND_WEBHOOK_SECRET 必须以 whsec_ 开头");
  need(!trim(c.resend_user_agent).empty(),
       "RESEND_USER_AGENT must not be empty (Cloudflare rejects requests without it) / RESEND_USER_AGENT 不能为空");
  need(c.resend_rate_rps > 0 && c.resend_rate_rps <= 100, "RESEND_RATE_RPS must be in (0, 100] / 速率必须在 0 到 100 之间");
  at_least(c.resend_timeout_sec, 1, "RESEND_TIMEOUT_SEC");
  at_least(c.webhook_tolerance_sec, 1, "AZMAIL_WEBHOOK_TOLERANCE_SEC");
  if (!c.ca_file.empty()) {
    std::error_code ec;
    need(std::filesystem::is_regular_file(c.ca_file, ec), "AZMAIL_CA_FILE does not exist / CA 文件不存在");
  }

  // ---- blob storage ---------------------------------------------------------------------------------
  if (c.blob_backend == BlobBackend::R2) {
    need(!c.r2_account_id.empty() || !c.r2_endpoint.empty(),
         "R2_ACCOUNT_ID (or R2_ENDPOINT) is required when AZMAIL_BLOB_BACKEND=r2 / 使用 R2 时必须设置 R2_ACCOUNT_ID");
    need(!c.r2_access_key_id.empty(), "R2_ACCESS_KEY_ID is required when AZMAIL_BLOB_BACKEND=r2 / 使用 R2 时必须设置 R2_ACCESS_KEY_ID");
    need(!c.r2_secret_access_key.empty(),
         "R2_SECRET_ACCESS_KEY is required when AZMAIL_BLOB_BACKEND=r2 / 使用 R2 时必须设置 R2_SECRET_ACCESS_KEY");
    need(!c.r2_bucket.empty(), "R2_BUCKET is required when AZMAIL_BLOB_BACKEND=r2 / 使用 R2 时必须设置 R2_BUCKET");
  }
  if (!c.r2_endpoint.empty()) {
    need(valid_http_url(c.r2_endpoint, false), "R2_ENDPOINT must be an http(s) origin without a path / R2_ENDPOINT 格式无效");
    need(is_https(c.r2_endpoint) || c.allow_insecure_http,
         "R2_ENDPOINT uses http:// but AZMAIL_ALLOW_INSECURE_HTTP is not set / R2_ENDPOINT 使用 http:// 时必须设置 AZMAIL_ALLOW_INSECURE_HTTP=1");
  }
  for (char ch : c.r2_bucket)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) {
      p.push_back("R2_BUCKET may only contain a-z, 0-9 and '-' / R2_BUCKET 只能包含小写字母、数字和 -");
      break;
    }
  need(c.r2_prefix.find("..") == std::string::npos && (c.r2_prefix.empty() || c.r2_prefix.front() != '/'),
       "R2_PREFIX must be a relative key prefix like 'azmail/' / R2_PREFIX 必须是相对前缀");
  need(c.r2_presign_ttl_sec >= 1 && c.r2_presign_ttl_sec <= 7 * 24 * 3600,
       "R2_PRESIGN_TTL_SEC must be 1..604800 / 预签名有效期必须在 1 秒到 7 天之间");
  at_least(c.file_cache_mb, std::size_t{1}, "AZMAIL_FILE_CACHE_MB");

  // ---- WebSocket ---------------------------------------------------------------------------------------
  at_least(c.ws_auth_timeout_ms, 100, "AZMAIL_WS_AUTH_TIMEOUT_MS");
  at_least(c.ws_max_message_bytes, std::size_t{256}, "AZMAIL_WS_MAX_MESSAGE_BYTES");
  at_least(c.ws_queue_cap, std::size_t{1}, "AZMAIL_WS_QUEUE_CAP");
  at_least(c.ws_max_sessions_per_user, std::size_t{1}, "AZMAIL_WS_MAX_PER_USER");
  return p;
}

std::vector<std::string> config_warnings(const Config& c) {
  std::vector<std::string> w;
  const int workers = c.db_threads + c.net_threads + c.files_threads + c.jobs_outbound_threads +
                      c.jobs_inbound_threads + c.jobs_sync_threads + c.jobs_maintenance_threads;
  if (c.db_pool_size < workers)
    w.push_back("AZMAIL_DB_POOL_SIZE (" + std::to_string(c.db_pool_size) + ") is below the number of worker threads (" +
                std::to_string(workers) + "); requests may wait for a database connection / 数据库连接数小于工作线程数");
  if (c.resend_webhook_secret.empty())
    w.push_back("RESEND_WEBHOOK_SECRET is not set: every webhook is rejected and mail arrives only via polling / "
                "未设置 RESEND_WEBHOOK_SECRET：所有 webhook 将被拒绝，只能依靠轮询收信");
  if (c.files_delivery == FilesDelivery::Redirect && c.blob_backend == BlobBackend::Local)
    w.push_back("AZMAIL_FILES_DELIVERY=redirect has no effect with the local blob backend / 本地存储时 redirect 无效");
  if (c.cors_origins.empty())
    w.push_back("AZMAIL_CORS_ORIGINS is empty: browsers on another origin cannot use the API or WebSocket / "
                "未配置跨域来源，浏览器无法跨域访问");
  if (c.allow_insecure_http)
    w.push_back("AZMAIL_ALLOW_INSECURE_HTTP=1 allows plain http:// to Resend/R2 (mock only) / 已允许不安全的 http 连接（仅限测试）");
  return w;
}

}  // namespace app

// ---- config.hpp contract -------------------------------------------------------------------------

Config load_config(const std::map<std::string, std::string>& env_overrides) {
  app::EnvMap file(env_overrides.begin(), env_overrides.end());
  auto loaded = app::config_from_env(app::merge_env(file, app::process_environment(), {}));
  for (const auto& w : loaded.warnings) log::warn(w);
  if (!loaded.problems.empty()) {
    std::string msg = "invalid configuration / 配置无效:";
    for (const auto& p : loaded.problems) msg += "\n  " + p;
    throw std::invalid_argument(msg);
  }
  return std::move(loaded.cfg);
}

std::vector<std::string> validate_config(const Config& cfg) {
  return app::validate_config(cfg, app::ConfigPurpose::Serve);
}

}  // namespace azm
