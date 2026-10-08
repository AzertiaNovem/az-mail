// Owner: WP-C
//
// Synchronous HTTPS client facade over async Beast (DESIGN A3 + Addendum A). Each send() runs
// a private io_context with co_spawn(..., use_future) so every operation has a real deadline
// (Beast's sync API ignores expires_after). Used from blocking pools / job threads only, never
// from an IO strand and never inside a DB transaction.
//  * TLS: SNI (SSL_set_tlsext_host_name) + ssl::host_name_verification, peer verification
//    against ClientOptions::ca_file or the system store. Uses asio::ssl::stream<beast::tcp_stream>.
//  * http:// only when ClientOptions::allow_insecure_http (mock); otherwise NetError(InsecureScheme).
//  * Never sends Accept-Encoding. Always sends User-Agent (ClientOptions::user_agent) unless the
//    request sets one.
//  * Redirects (301/302/303/307/308) followed up to HttpRequest::max_redirects; on a host change
//    the Authorization header is dropped (S3 presigned URLs, mock /_dl → /_blob); 303 turns into
//    GET without a body.
//  * Request body: `body` (string) or `body_file` (streamed from disk, Content-Length = file size).
//  * Response body: into HttpResponse::body (≤ max_body, else NetError(TooLarge)) or, when `sink`
//    is set and the final status is 2xx, streamed to that file (≤ max_body) with sha256 + size
//    computed while writing. Non-2xx responses always go to `body` (truncated to 64 KiB) so
//    callers can parse error documents; the sink file is then not created.
//  * Shutdown (additive, RT-7): ClientOptions::cancel shares a CancelSignal; cancel() aborts every
//    send() in flight within milliseconds (NetError{Kind::Aborted}) and fails later sends fast.
// Thread-safe: concurrent send() calls are independent.
#pragma once

#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azm {
struct Config;
}

namespace azm::net {

// ---- additive (RT-7): process shutdown --------------------------------------------------------
// Abort switch shared by the HttpClients whose ClientOptions::cancel points to it (App: one per
// process, fired once the job grace period of a graceful shutdown is over). cancel() stops every
// send() in flight — it throws NetError{Kind::Aborted} within milliseconds, whatever the request
// deadline, DNS lookups included — and every later send() fails the same way before connecting.
// Irreversible. Thread-safe.
class CancelSignal {
 public:
  CancelSignal();
  ~CancelSignal();
  CancelSignal(const CancelSignal&) = delete;
  CancelSignal& operator=(const CancelSignal&) = delete;

  void cancel() noexcept;
  bool cancelled() const noexcept;

 private:
  friend class HttpClient;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ClientOptions {
  std::string user_agent = "azmail";       // cfg.resend_user_agent (Cloudflare 1010 without it)
  std::string ca_file;                     // cfg.ca_file; empty = system default trust store
  bool allow_insecure_http = false;        // cfg.allow_insecure_http (AZMAIL_ALLOW_INSECURE_HTTP=1)
  std::chrono::milliseconds connect_timeout{10000};  // DNS + TCP + TLS handshake
  std::chrono::milliseconds read_timeout{30000};     // idle limit per read/write (re-armed per chunk)
  std::shared_ptr<CancelSignal> cancel;              // additive (RT-7): shutdown abort; null = never
};

// Options derived from Config (user agent, CA file, insecure flag, cfg.resend_timeout_sec as
// read_timeout).
ClientOptions client_options_from(const Config& cfg);

struct HttpRequest {
  boost::beast::http::verb method = boost::beast::http::verb::get;
  std::string url;  // absolute http(s) URL; path and query must already be percent-encoded
  std::vector<std::pair<std::string, std::string>> headers;  // sent as given (Host is derived)
  std::string body;                                   // request body (empty = none)
  std::optional<std::filesystem::path> body_file;     // streamed request body (Addendum A); `body` must be empty
  std::chrono::milliseconds timeout{30000};           // overall deadline for the whole exchange incl. redirects
  int max_redirects = 0;                              // 0 = return 3xx responses as-is
  std::optional<std::filesystem::path> sink;          // write a 2xx response body to this file
  std::size_t max_body = 50u << 20;                   // response body limit (memory or sink)
  // Additive (RT-4): wait for the response header once the request is written, instead of
  // ClientOptions::read_timeout (still capped by `timeout`). For large uploads: the last
  // write completes into the kernel's send buffer, which then drains at the link's pace.
  std::optional<std::chrono::milliseconds> response_timeout;
};

struct HttpResponse {
  unsigned status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // lowercased names, in order
  std::string body;       // empty when streamed to the sink
  std::string final_url;  // URL of the last request after redirects
  // Set when the body was streamed to HttpRequest::sink (Addendum A).
  std::optional<std::string> sink_sha256;  // lowercase hex sha256 of the written file
  std::uint64_t body_size = 0;             // bytes received (body or sink)

  // First header value with that name (case-insensitive); nullopt when absent.
  std::optional<std::string> header(std::string_view name) const;
};

struct NetError : std::runtime_error {
  enum class Kind {
    Timeout,         // deadline / idle timeout
    Connect,         // DNS / TCP failure
    Tls,             // handshake / verification failure
    Protocol,        // malformed HTTP, too many redirects
    TooLarge,        // response body over max_body
    InsecureScheme,  // http:// without allow_insecure_http, or unsupported scheme
    Io,              // local file error (body_file / sink)
    Aborted,         // additive (RT-7): ClientOptions::cancel fired (process shutdown)
  };
  NetError(Kind k, const std::string& what) : std::runtime_error(what), kind(k) {}
  Kind kind;
  // Transient conditions worth retrying (Timeout, Connect, Protocol; Aborted: later, by a job
  // after the restart — never in a loop within this process).
  bool retryable() const {
    return kind == Kind::Timeout || kind == Kind::Connect || kind == Kind::Protocol || kind == Kind::Aborted;
  }
};

class HttpClient {
 public:
  explicit HttpClient(ClientOptions);
  virtual ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // Performs the request. Returns any HTTP status (4xx/5xx are not exceptions). Throws NetError
  // for transport failures. Thread-safe. Virtual so tests can substitute a fake transport
  // (e.g. R2BlobStore and resend::Client unit tests).
  virtual HttpResponse send(const HttpRequest&);

  const ClientOptions& options() const;
  // Additive (RT-7): true once options().cancel fired (in-process retry loops stop retrying).
  bool cancelled() const;

 protected:
  HttpClient();  // for test doubles that override send()

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::net
