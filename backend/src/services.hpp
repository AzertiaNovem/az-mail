// Owner: WP0 (frozen; additive changes by WP-A, which builds the instance in app/app.cpp)
//
// Services: the aggregate of long-lived singletons passed to HTTP handlers (http::Ctx::svc) and
// job handlers (jobs::JobFn). It owns nothing; App owns every referenced object and outlives
// all users. Tests build one from real lightweight objects (temp-file db::Pool, LocalBlobStore
// in a TempDir, ManualClock, RecordingNotifier) and leave the optional pointers null.
#pragma once

#include "config.hpp"
#include "core/blob_store.hpp"
#include "core/errors.hpp"
#include "core/signed_url.hpp"
#include "core/time.hpp"
#include "notifier.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace boost::asio {
class thread_pool;
}

namespace azm {

namespace db {
class Pool;
}
namespace net {
class HttpClient;
}
namespace resend {
class Client;
class RateLimiter;
}  // namespace resend
namespace jobs {
class Runner;
}
namespace http {
class LoginThrottle;
struct SessionResolver;
}

struct Services {
  // ---- always present ------------------------------------------------------------------------
  const Config& cfg;
  db::Pool& db;                  // SQLite pool (TxHooks: notifier + runner wake)
  BlobStore& blobs;              // primary store: every new blob is written here (Addendum A)
  const SignedUrls& signed_urls;  // HMAC file/raw URLs (secret = cfg.server_secret)
  Notifier& notifier;            // ws::Hub in production; publish only via Tx::emit
  const Clock& clock;            // SystemClock in production, ManualClock in tests

  // ---- optional (null in tests that don't need them) ----------------------------------------
  resend::Client* resend = nullptr;            // Resend API (network; never inside a Tx)
  net::HttpClient* http = nullptr;             // shared outbound HTTPS client
  resend::RateLimiter* rate_limiter = nullptr;  // shared by resend::Client
  jobs::Runner* runner = nullptr;              // job runner (wake(); handlers registered at start)
  http::LoginThrottle* login_throttle = nullptr;  // login throttling + scrypt semaphore
  // The other backend (mixed store, Addendum A): App wires the local store when
  // cfg.blob_backend == R2, and the R2 store when backend == Local and R2 is configured.
  BlobStore* secondary_blobs = nullptr;
  boost::asio::thread_pool* db_workers = nullptr;   // blocking pool "db"  (Exec::Db)
  boost::asio::thread_pool* net_workers = nullptr;  // blocking pool "net" (Exec::Net)
  boost::asio::thread_pool* files_workers = nullptr;  // blocking pool "files" (Exec::Files)
  // Additive (WP-A): replaces the repo-backed Bearer-token lookup in http::authenticate when
  // set (unit tests of the HTTP/WS runtime; null in production → repo::find_session).
  http::SessionResolver* session_resolver = nullptr;

  int64_t now_ms() const { return clock.now_ms(); }

  // Store holding a blob whose `blobs.storage` column is `storage` ("local"|"r2"): the primary
  // when kinds match, else the secondary when it matches. Throws BlobError{retryable=false}
  // when neither matches (misconfiguration: never silently read from the wrong store).
  BlobStore& blobs_for(std::string_view storage) const {
    if (storage == blobs.kind()) return blobs;
    if (secondary_blobs != nullptr && secondary_blobs->kind() == storage) return *secondary_blobs;
    BlobError e("no blob store configured for storage '" + std::string(storage) + "'");
    e.retryable = false;
    throw e;
  }

  // Resend client or ApiError(503, "service_unavailable") when not configured.
  resend::Client& resend_client() const {
    if (resend == nullptr) throw ApiError::unavailable("service_unavailable", "邮件服务未配置");
    return *resend;
  }
};

}  // namespace azm
