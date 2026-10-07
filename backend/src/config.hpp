// Runtime configuration (pure data). Loaded by WP-A (app/config.cpp) from the environment
// (AZMAIL_* / RESEND_* / R2_*), an optional --env-file and CLI flags; the env name of each field
// is given in its comment. Defaults are suitable for local development.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace azm {

enum class ScheduleMode { Resend, Local };      // AZMAIL_SCHEDULE_MODE=resend|local
enum class BlobBackend { Local, R2 };           // AZMAIL_BLOB_BACKEND=local|r2
enum class FilesDelivery { Redirect, Proxy };   // AZMAIL_FILES_DELIVERY=redirect|proxy

// Wire/env spelling of the enums ("resend"/"local", "local"/"r2", "redirect"/"proxy").

inline std::string_view enum_name(ScheduleMode m) { return m == ScheduleMode::Local ? "local" : "resend"; }
inline std::string_view enum_name(BlobBackend b) { return b == BlobBackend::R2 ? "r2" : "local"; }
inline std::string_view enum_name(FilesDelivery d) {
  return d == FilesDelivery::Proxy ? "proxy" : "redirect";
}

struct Config {
  // ---- HTTP server -------------------------------------------------------------------------
  std::string listen_address = "127.0.0.1";  // AZMAIL_LISTEN_ADDRESS
  uint16_t listen_port = 8080;               // AZMAIL_PORT
  // Public base URL of the API origin (signed file URLs, Me.server.files_origins), no trailing
  // slash, e.g. "https://mail-api.example.com".
  std::string public_api_base_url = "http://127.0.0.1:8080";  // AZMAIL_PUBLIC_API_URL
  std::vector<std::string> cors_origins = {"http://localhost:5173",
                                           "http://127.0.0.1:5173"};  // AZMAIL_CORS_ORIGINS (comma list)
  std::vector<std::string> trusted_proxies = {"127.0.0.1", "::1"};    // AZMAIL_TRUSTED_PROXIES (comma list; IPs/CIDRs)
  std::size_t inflight_cap = 256;            // AZMAIL_MAX_INFLIGHT (503 above)
  std::size_t max_connections = 4096;        // AZMAIL_MAX_CONNECTIONS
  int header_timeout_sec = 15;               // AZMAIL_HEADER_TIMEOUT_SEC
  int body_idle_timeout_sec = 30;            // AZMAIL_BODY_IDLE_TIMEOUT_SEC (re-armed per chunk)
  int keepalive_timeout_sec = 75;            // AZMAIL_KEEPALIVE_TIMEOUT_SEC

  // ---- threads / pools ---------------------------------------------------------------------
  int io_threads = 2;        // AZMAIL_IO_THREADS
  int db_threads = 8;        // AZMAIL_DB_THREADS    (blocking handler pool "db")
  int net_threads = 2;       // AZMAIL_NET_THREADS   (pool "net": synchronous Resend calls)
  int files_threads = 8;     // AZMAIL_FILES_THREADS (pool "files": R2/disk file I/O, Exec::Files)
  // AZMAIL_DB_POOL_SIZE (SQLite connections): ≥ db + net + files + all job lane threads
  // (8 + 2 + 8 + 6 = 24) so no worker waits for a connection.
  int db_pool_size = 24;
  int jobs_outbound_threads = 2;     // AZMAIL_JOBS_OUTBOUND_THREADS
  int jobs_inbound_threads = 2;      // AZMAIL_JOBS_INBOUND_THREADS
  int jobs_sync_threads = 1;         // AZMAIL_JOBS_SYNC_THREADS
  int jobs_maintenance_threads = 1;  // AZMAIL_JOBS_MAINTENANCE_THREADS
  int shutdown_grace_sec = 25;       // AZMAIL_SHUTDOWN_GRACE_SEC

  // ---- storage -----------------------------------------------------------------------------
  std::string data_dir = "data";           // AZMAIL_DATA_DIR (blobs/, tmp/, cache/)
  std::string db_path = "data/azmail.db";  // AZMAIL_DB_PATH (local disk, never NFS)

  // ---- security ----------------------------------------------------------------------------
  // HMAC secret for signed URLs etc. (>= 32 random bytes; hex/base64 decoding is up to the
  // loader). Required in production.
  std::string server_secret;            // AZMAIL_SECRET
  int session_ttl_days = 30;            // AZMAIL_SESSION_TTL_DAYS (sliding)
  int session_touch_interval_sec = 3600;  // AZMAIL_SESSION_TOUCH_SEC (touch at most hourly)
  int login_max_per_email = 5;          // AZMAIL_LOGIN_MAX_PER_EMAIL  (per window)
  int login_max_per_ip = 20;            // AZMAIL_LOGIN_MAX_PER_IP     (per window)
  int login_window_sec = 900;           // AZMAIL_LOGIN_WINDOW_SEC (15 min)
  int scrypt_concurrency = 4;           // AZMAIL_SCRYPT_CONCURRENCY
  // AZMAIL_SIGNED_URL_TTL_SEC: App builds SignedUrls(secret, public_api_base_url, ttl*1000);
  // read-time URLs use SignedUrls::expiry(now) (hour-rounded only when ttl ≥ 1 h).
  int64_t signed_url_ttl_sec = 12 * 3600;

  // ---- mail domain -------------------------------------------------------------------------
  // Local (team) domains; the `domains` table is authoritative at runtime, this list is used to
  // bootstrap/validate (e.g. `azmail add-domain`, mock wiring).
  std::vector<std::string> local_domains;  // AZMAIL_LOCAL_DOMAINS (comma list)
  // AZMAIL_UNDO_SEND_SECONDS (0|5|10|20|30): initial user_settings.undo_send_seconds of new
  // users (repo::NewUser::undo_send_seconds, set by `azmail create-user` and admin create).
  int default_undo_send_seconds = 5;
  ScheduleMode schedule_mode = ScheduleMode::Resend;  // AZMAIL_SCHEDULE_MODE
  int schedule_min_lead_sec = 60;          // AZMAIL_SCHEDULE_MIN_LEAD_SEC (now+60 s)
  int schedule_max_days = 30;              // AZMAIL_SCHEDULE_MAX_DAYS (now+30 d)
  int max_recipients_per_field = 50;       // AZMAIL_MAX_RECIPIENTS (To, Cc, Bcc each)
  bool unroutable_to_admins = false;       // AZMAIL_UNROUTABLE_TO_ADMINS (deliver unknown local rcpt to admins)

  // ---- size limits (bytes) -----------------------------------------------------------------
  std::size_t max_header_bytes = 16 * 1024;              // AZMAIL_MAX_HEADER_BYTES
  std::size_t json_body_limit = 1u << 20;                // AZMAIL_JSON_BODY_LIMIT (1 MiB)
  std::size_t draft_body_limit = 8u << 20;               // AZMAIL_DRAFT_BODY_LIMIT (8 MiB)
  std::size_t upload_body_limit = 25u << 20;             // AZMAIL_UPLOAD_LIMIT (25 MiB per file)
  std::size_t webhook_body_limit = 1u << 20;             // AZMAIL_WEBHOOK_BODY_LIMIT (1 MiB)
  std::size_t max_message_attachment_bytes = 28u << 20;  // AZMAIL_MAX_MESSAGE_ATTACHMENTS (28 MiB raw)
  std::size_t inbound_attachment_limit = 50u << 20;      // AZMAIL_INBOUND_ATTACHMENT_LIMIT (50 MB)
  std::size_t inbound_raw_limit = 60u << 20;             // AZMAIL_INBOUND_RAW_LIMIT
  // Informational only, NOT configurable: fts_reindex always truncates to mail::kFtsBodyLimit
  // (256 KB, DESIGN §2); there is no AZMAIL_FTS_BODY_LIMIT and loaders must not add one.
  std::size_t fts_body_limit = 256u << 10;

  // ---- retention / maintenance ------------------------------------------------------------
  int trash_purge_days = 30;           // AZMAIL_TRASH_PURGE_DAYS
  int spam_purge_days = 30;            // AZMAIL_SPAM_PURGE_DAYS
  int unattached_upload_ttl_hours = 24;  // AZMAIL_UPLOAD_TTL_HOURS (orphan uploads)
  int blob_gc_grace_hours = 24;        // AZMAIL_BLOB_GC_GRACE_HOURS (unreferenced blobs)
  int jobs_done_retention_days = 7;    // AZMAIL_JOBS_RETENTION_DAYS
  int webhook_events_retention_days = 30;  // AZMAIL_WEBHOOK_RETENTION_DAYS
  int poll_interval_sec = 120;         // AZMAIL_POLL_INTERVAL_SEC (poll.receiving)
  int reconcile_interval_sec = 600;    // AZMAIL_RECONCILE_INTERVAL_SEC (outbound.reconcile)

  // ---- Resend ------------------------------------------------------------------------------
  std::string resend_api_key;                          // RESEND_API_KEY
  std::string resend_api_base = "https://api.resend.com";  // RESEND_API_BASE (mock: http://127.0.0.1:<port>)
  std::string resend_webhook_secret;                   // RESEND_WEBHOOK_SECRET ("whsec_...")
  std::string resend_user_agent = "azmail/0.1.0";      // RESEND_USER_AGENT (required by Cloudflare)
  double resend_rate_rps = 8.0;                        // RESEND_RATE_RPS (team-wide limit is 10)
  int resend_timeout_sec = 30;                         // RESEND_TIMEOUT_SEC
  int webhook_tolerance_sec = 300;                     // AZMAIL_WEBHOOK_TOLERANCE_SEC (Svix)

  // ---- outbound HTTP client ---------------------------------------------------------------
  bool allow_insecure_http = false;  // AZMAIL_ALLOW_INSECURE_HTTP=1 (mock only)
  std::string ca_file;               // AZMAIL_CA_FILE (empty = system default trust store)

  // ---- blob storage (Addendum A) ------------------------------------------------------------
  BlobBackend blob_backend = BlobBackend::Local;            // AZMAIL_BLOB_BACKEND
  std::string r2_account_id;                                // R2_ACCOUNT_ID
  std::string r2_access_key_id;                             // R2_ACCESS_KEY_ID
  std::string r2_secret_access_key;                         // R2_SECRET_ACCESS_KEY
  std::string r2_bucket;                                    // R2_BUCKET
  std::string r2_endpoint;  // R2_ENDPOINT (empty → https://<account>.r2.cloudflarestorage.com)
  std::string r2_prefix = "azmail/";                        // R2_PREFIX
  // AZMAIL_FILES_DELIVERY (default proxy, Addendum A: *.r2.cloudflarestorage.com is often slow
  // from mainland China). App falls back to proxy when the presign-override probe fails.
  FilesDelivery files_delivery = FilesDelivery::Proxy;
  int r2_presign_ttl_sec = 300;                             // R2_PRESIGN_TTL_SEC
  std::size_t file_cache_mb = 1024;                         // AZMAIL_FILE_CACHE_MB (proxy mode)

  // ---- logging -----------------------------------------------------------------------------
  std::string log_level = "info";  // AZMAIL_LOG_LEVEL (trace|debug|info|warn|error)

  // ---- WebSocket ---------------------------------------------------------------------------
  int ws_auth_timeout_ms = 5000;           // AZMAIL_WS_AUTH_TIMEOUT_MS
  std::size_t ws_max_message_bytes = 16 * 1024;  // AZMAIL_WS_MAX_MESSAGE_BYTES
  std::size_t ws_queue_cap = 512;          // AZMAIL_WS_QUEUE_CAP (overflow closes the socket)
  std::size_t ws_max_sessions_per_user = 32;  // AZMAIL_WS_MAX_PER_USER

  // R2 endpoint actually used (explicit R2_ENDPOINT or derived from the account id).
  std::string effective_r2_endpoint() const {
    if (!r2_endpoint.empty()) return r2_endpoint;
    if (r2_account_id.empty()) return {};
    return "https://" + r2_account_id + ".r2.cloudflarestorage.com";
  }
};

// Implemented by WP-A (src/app/config.cpp). `env_overrides` holds KEY=VALUE pairs from
// --env-file; real environment variables take precedence over them.
Config load_config(const std::map<std::string, std::string>& env_overrides);
// Human-readable problems (empty = valid), e.g. missing AZMAIL_SECRET or R2 credentials.
std::vector<std::string> validate_config(const Config& cfg);

}  // namespace azm
