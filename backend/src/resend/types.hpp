// Owner: WP-C
//
// Resend API DTOs, error model and webhook envelope (DESIGN §3 "resend/client.hpp" types,
// B3–B9). All times are ms epoch UTC (Resend timestamps parsed with parse_iso8601_lenient).
// resend/client.hpp includes this header, so the §3 spellings (resend::SendRequest, …) hold.
#pragma once

#include <boost/json/object.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azm::resend {

// Rate-limiter priority (B6): send > inbound fetch > meta/poll/reconcile.
enum class Priority { High, Normal, Low };

// Error from a Resend call (HTTP status + Resend `name`), or a transport failure (Network).
struct Error : std::exception {
  enum class Kind {
    RateLimited,          // 429 rate_limit_exceeded → retry after `retry_after`, attempt not counted
    Quota,                // 429 daily_quota_exceeded / monthly_quota_exceeded → failed, no auto-retry
    Validation,           // 400 / 422 (validation_error, invalid_*, missing_required_field, …)
    Auth,                 // 401 / 403 (incl. Cloudflare 1010 non-JSON 403 = missing User-Agent)
    NotFound,             // 404
    IdempotencyConflict,  // 409 invalid_idempotent_request (same key, different body)
    IdempotencyInFlight,  // 409 concurrent_idempotent_requests → retry after ~2 s
    Server,               // 5xx / unexpected
    Network,              // net::NetError (timeout, connect, TLS, …)
  } kind = Kind::Server;
  int http_status = 0;    // 0 for Network
  std::string name;       // Resend error name, e.g. "rate_limit_exceeded" ("" when absent)
  std::string message;    // Resend message (English, safe to log; never contains secrets)
  std::optional<std::chrono::seconds> retry_after;  // from the retry-after header (429)

  Error() = default;
  Error(Kind k, int status, std::string name_, std::string message_,
        std::optional<std::chrono::seconds> retry_after_ = std::nullopt)
      : kind(k), http_status(status), name(std::move(name_)), message(std::move(message_)),
        retry_after(retry_after_) {}

  // RateLimited, IdempotencyInFlight, Server and Network are transient.
  bool retryable() const {
    return kind == Kind::RateLimited || kind == Kind::IdempotencyInFlight ||
           kind == Kind::Server || kind == Kind::Network;
  }
  const char* what() const noexcept override {
    return name.empty() ? "resend_error" : name.c_str();
  }
};

// "rate_limited", "quota", "validation", "auth", "not_found", "idempotency_conflict",
// "idempotency_in_flight", "server", "network".
std::string_view to_string(Error::Kind k);

// Maps an HTTP error response to an Error (pure; unit-tested by WP-C):
//   429 + name rate_limit_exceeded → RateLimited (retry_after from the header, default 1 s);
//   429 + daily_/monthly_quota_exceeded → Quota; 409 invalid_idempotent_request →
//   IdempotencyConflict; 409 concurrent_idempotent_requests → IdempotencyInFlight;
//   401/403 → Auth (a non-JSON 403 body is reported with name "cloudflare_1010");
//   404 → NotFound; 400/422 → Validation; anything else → Server.
// `body` is the raw response body (JSON {statusCode,name,message} or arbitrary text).
Error classify_error(int http_status, std::string_view body,
                     std::optional<std::string_view> retry_after_header);

// ---- sending ---------------------------------------------------------------------------------
struct OutAttachment {
  std::string filename;
  std::string content_type;
  std::string content_b64;                 // base64 of the bytes (no line breaks)
  std::optional<std::string> content_id;   // inline images (referenced as cid:<content_id>)
};

struct SendRequest {
  std::string from;                        // formatted "Name <addr>" (core format_address)
  std::vector<std::string> to, cc, bcc, reply_to;  // formatted addresses
  std::string subject, html, text;
  std::vector<std::pair<std::string, std::string>> headers;  // In-Reply-To, References, X-AzMail-Ref
  std::vector<std::pair<std::string, std::string>> tags;     // name/value: [A-Za-z0-9_-], ≤ 256
  std::vector<OutAttachment> attachments;
  std::optional<std::string> scheduled_at_iso;  // "…Z" ISO-8601 UTC (B5), never natural language
  std::string idempotency_key;                  // outbound.uuid (B1)
};

// GET /emails/{id}
struct SentEmail {
  std::string id;
  std::optional<std::string> message_id;  // RFC 5322 Message-ID as returned (may carry <>)
  std::optional<std::string> last_event;  // "sent", "delivered", "bounced", "scheduled", …
  std::optional<int64_t> scheduled_at_ms;
  int64_t created_at_ms = 0;
};

// ---- receiving -------------------------------------------------------------------------------
struct Auth {
  std::optional<std::string> spf, dkim, dmarc;  // "pass" | "fail" | … (lowercase), absent = unknown
};

struct RecvAttachment {
  std::string id;
  std::string filename;
  std::string content_type;
  std::string content_disposition;         // "inline" | "attachment"
  std::optional<std::string> content_id;   // without <>
  int64_t size = 0;
  std::string download_url;                // expires after ~1 h (B7): re-list before downloading
};

// GET /emails/receiving/{id}?html_format=cid
struct ReceivedEmail {
  std::string id;
  std::string from;     // raw From header value ("Name <a@b>")
  std::string subject;  // as returned (decoded by Resend if F4.7 holds; eml decoding is the fallback)
  std::string message_id;
  std::vector<std::string> to, cc, bcc, reply_to;  // raw address strings
  std::vector<std::string> received_for;           // envelope recipients (C1)
  std::optional<std::string> html, text;           // html references inline parts as cid:
  std::vector<std::pair<std::string, std::string>> headers;  // lowercased names
  Auth auth;
  std::optional<std::string> raw_download_url;  // raw .eml (expires ~1 h)
  int64_t created_at_ms = 0;
  std::vector<RecvAttachment> attachments;
};

// GET /emails/receiving?limit&after&before — ids newest-first per the mock (B8: never rely on it)
struct ReceivedPage {
  std::vector<std::string> ids;
  bool has_more = false;
};

// ---- domains (admin GET /api/admin/domains/:id/status) ---------------------------------------
struct DomainRecord {
  std::string record;  // "SPF" | "DKIM" | "MX" | …
  std::string name;
  std::string type;    // "TXT" | "MX" | "CNAME"
  std::string ttl;
  std::string status;  // "verified" | "pending" | …
  std::string value;
  std::optional<int> priority;  // MX only; omitted from DomainStatus JSON when absent (api/dto.hpp)
};

struct DomainInfo {
  std::string id;
  std::string name;
  std::string status;                 // not_started | pending | verified | failed | temporary_failure
  std::optional<std::string> region;  // e.g. "us-east-1"
  std::optional<int64_t> created_at_ms;
  std::vector<DomainRecord> records;  // empty in list_domains(); filled by get_domain()
};

// ---- download -------------------------------------------------------------------------------
struct DownloadResult {
  int64_t size = 0;
  std::string sha256;  // lowercase hex of the downloaded bytes
};

// ---- webhooks --------------------------------------------------------------------------------
// Parsed Resend webhook body {type, created_at, data:{…}} (after Svix verification).
struct WebhookEnvelope {
  std::string type;            // "email.sent", "email.delivered", …, "email.received"
  int64_t created_at_ms = 0;   // envelope created_at (0 when absent/unparseable)
  boost::json::object data;    // the raw data object
  std::optional<std::string> email_id;    // data.email_id (outbound: resend id; received: receiving id)
  std::optional<std::string> message_id;  // data.message_id when present
  std::vector<std::string> to;            // data.to (all recipients; events are per email, B3)
  std::optional<std::string> from;        // data.from
  std::optional<std::string> subject;     // data.subject
  std::map<std::string, std::string> tags;  // data.tags: map form, or [{name,value}] normalized to a map

  bool is_received() const { return type == "email.received"; }
  // Delivery-status events for mail we sent ("email.*" except email.received).
  bool is_outbound_event() const { return type.rfind("email.", 0) == 0 && !is_received(); }
};

// Parses a webhook body. Throws std::invalid_argument when it is not a JSON object with a
// string "type" and an object "data".
WebhookEnvelope parse_webhook(std::string_view body);

}  // namespace azm::resend
