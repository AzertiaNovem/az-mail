// Owner: WP-C
// Resend error classification and webhook envelope parsing (DESIGN B3–B9).
#include "resend/types.hpp"

#include "core/strings.hpp"
#include "core/time.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <charconv>
#include <cmath>
#include <stdexcept>

namespace azm::resend {
namespace {

std::optional<std::chrono::seconds> parse_retry_after(std::optional<std::string_view> header) {
  if (!header) return std::nullopt;
  const std::string_view v = trim(*header);
  if (v.empty()) return std::nullopt;
  // Delta-seconds (possibly fractional, e.g. "0.5"); HTTP-date values are not used by Resend.
  double d = 0;
  auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
  if (ec != std::errc() || p != v.data() + v.size() || !std::isfinite(d) || d < 0) return std::nullopt;
  const auto secs = static_cast<int64_t>(std::ceil(d));
  return std::chrono::seconds(std::min<int64_t>(secs, 24 * 3600));
}

std::string text_snippet(std::string_view body) {
  std::string s = utf8_truncate(trim(body), 200);
  for (char& c : s)
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  return s;
}

bool is_api_key_error(std::string_view name) {
  return name == "missing_api_key" || name == "invalid_api_key" || name == "restricted_api_key";
}

bool is_validation_name(std::string_view name) {
  return name == "validation_error" || name == "missing_required_field" ||
         (name.rfind("invalid_", 0) == 0 && !is_api_key_error(name) && name != "invalid_idempotent_request");
}

}  // namespace

std::string_view to_string(Error::Kind k) {
  switch (k) {
    case Error::Kind::RateLimited: return "rate_limited";
    case Error::Kind::Quota: return "quota";
    case Error::Kind::Validation: return "validation";
    case Error::Kind::Auth: return "auth";
    case Error::Kind::NotFound: return "not_found";
    case Error::Kind::IdempotencyConflict: return "idempotency_conflict";
    case Error::Kind::IdempotencyInFlight: return "idempotency_in_flight";
    case Error::Kind::Server: return "server";
    case Error::Kind::Network: return "network";
  }
  return "server";
}

Error classify_error(int http_status, std::string_view body,
                     std::optional<std::string_view> retry_after_header) {
  using K = Error::Kind;
  std::string name, message;
  bool json = false;
  {
    boost::system::error_code ec;
    const boost::json::value v = boost::json::parse(body, ec);
    if (!ec && v.is_object()) {
      json = true;
      const auto& o = v.as_object();
      if (auto it = o.find("name"); it != o.end() && it->value().is_string())
        name = std::string(it->value().as_string());
      if (auto it = o.find("message"); it != o.end() && it->value().is_string())
        message = text_snippet(it->value().as_string());
      else if (auto e = o.find("error"); e != o.end() && e->value().is_string())
        message = text_snippet(e->value().as_string());
    }
  }
  if (!json) message = text_snippet(body);
  const auto retry_after = parse_retry_after(retry_after_header);

  if (http_status == 429) {
    if (name == "daily_quota_exceeded" || name == "monthly_quota_exceeded")
      return Error(K::Quota, http_status, name, message);
    if (name.empty()) name = "rate_limit_exceeded";
    return Error(K::RateLimited, http_status, name, message, retry_after.value_or(std::chrono::seconds(1)));
  }
  if (name == "invalid_idempotent_request") return Error(K::IdempotencyConflict, http_status, name, message);
  if (name == "concurrent_idempotent_requests") return Error(K::IdempotencyInFlight, http_status, name, message);
  if (http_status == 401 || is_api_key_error(name)) return Error(K::Auth, http_status, name, message);
  if (http_status >= 400 && http_status < 500 && is_validation_name(name))
    return Error(K::Validation, http_status, name, message);
  if (http_status == 403) {
    if (!json) {
      // Cloudflare in front of Resend answers "error code: 1010" (plain text) when the
      // User-Agent is missing or blocked: a configuration bug, never retried (B6).
      return Error(K::Auth, http_status, "cloudflare_1010",
                   "403 with a non-JSON body (Cloudflare 1010?): check RESEND_USER_AGENT; requests "
                   "without a User-Agent are blocked. Body: " + message);
    }
    return Error(K::Auth, http_status, name, message);
  }
  if (http_status == 404) return Error(K::NotFound, http_status, name, message);
  if (http_status == 409) return Error(K::IdempotencyConflict, http_status, name, message);
  if (http_status >= 400 && http_status < 500) return Error(K::Validation, http_status, name, message);
  return Error(K::Server, http_status, name, message);
}

namespace {

std::optional<std::string> str_field(const boost::json::object& o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().as_string());
}

// A string, or an array of strings (Resend uses both shapes for address fields).
std::vector<std::string> str_list(const boost::json::object& o, std::string_view key) {
  std::vector<std::string> out;
  auto it = o.find(key);
  if (it == o.end()) return out;
  if (it->value().is_string()) {
    out.emplace_back(it->value().as_string());
  } else if (it->value().is_array()) {
    for (const auto& v : it->value().as_array())
      if (v.is_string()) out.emplace_back(v.as_string());
  }
  return out;
}

std::string scalar_text(const boost::json::value& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_int64()) return std::to_string(v.as_int64());
  if (v.is_uint64()) return std::to_string(v.as_uint64());
  if (v.is_bool()) return v.as_bool() ? "true" : "false";
  if (v.is_double()) return boost::json::serialize(v);
  return {};
}

}  // namespace

WebhookEnvelope parse_webhook(std::string_view body) {
  boost::system::error_code ec;
  boost::json::parse_options po;
  po.max_depth = 64;
  boost::json::value v = boost::json::parse(body, ec, {}, po);
  if (ec || !v.is_object()) throw std::invalid_argument("webhook: body is not a JSON object");
  auto& o = v.as_object();
  WebhookEnvelope env;
  auto type = o.find("type");
  if (type == o.end() || !type->value().is_string() || type->value().as_string().empty())
    throw std::invalid_argument("webhook: missing type");
  env.type = std::string(type->value().as_string());
  auto data = o.find("data");
  if (data == o.end() || !data->value().is_object()) throw std::invalid_argument("webhook: missing data object");
  env.data = data->value().as_object();
  if (auto ca = str_field(o, "created_at")) env.created_at_ms = parse_iso8601_lenient(*ca).value_or(0);

  const auto& d = env.data;
  if (auto id = str_field(d, "email_id"); id && !id->empty()) env.email_id = std::move(id);
  if (auto mid = str_field(d, "message_id"); mid && !mid->empty()) env.message_id = std::move(mid);
  env.to = str_list(d, "to");
  if (auto from = str_field(d, "from")) env.from = std::move(from);
  if (auto subject = str_field(d, "subject")) env.subject = std::move(subject);
  if (auto tags = d.find("tags"); tags != d.end()) {
    if (tags->value().is_object()) {
      for (const auto& [k, tv] : tags->value().as_object()) env.tags[std::string(k)] = scalar_text(tv);
    } else if (tags->value().is_array()) {
      for (const auto& t : tags->value().as_array()) {
        if (!t.is_object()) continue;
        const auto& to = t.as_object();
        auto n = to.find("name");
        auto val = to.find("value");
        if (n == to.end() || !n->value().is_string()) continue;
        env.tags[std::string(n->value().as_string())] = val == to.end() ? "" : scalar_text(val->value());
      }
    }
  }
  return env;
}

}  // namespace azm::resend
