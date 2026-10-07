// Owner: WP-A
// CORS allowlist, preflight and response decoration (see cors.hpp).
#include "http/cors.hpp"

#include "config.hpp"
#include "core/strings.hpp"

#include <boost/url/parse.hpp>

#include <charconv>

namespace azm::http {

std::optional<std::string> normalize_origin(std::string_view origin) {
  origin = trim(origin);
  if (origin.empty() || origin == "null") return std::nullopt;
  auto parsed = boost::urls::parse_absolute_uri(boost::core::string_view(origin.data(), origin.size()));
  if (!parsed) return std::nullopt;
  const auto& u = *parsed;
  const std::string scheme = to_lower_ascii(std::string_view(u.scheme()));
  if (scheme != "http" && scheme != "https") return std::nullopt;
  if (!u.has_authority() || u.has_userinfo() || u.has_query() || u.has_fragment()) return std::nullopt;
  if (!u.encoded_path().empty()) return std::nullopt;
  const std::string host = to_lower_ascii(std::string_view(u.encoded_host()));
  if (host.empty()) return std::nullopt;
  std::string out = scheme + "://" + host;
  if (u.has_port()) {
    const std::string_view port(u.port());
    unsigned value = 0;
    auto [p, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || ec != std::errc() || p != port.data() + port.size() || value == 0 || value > 65535)
      return std::nullopt;
    out += ':';
    out += port;
  }
  return out;
}

CorsPolicy cors_policy_from(const Config& cfg) {
  CorsPolicy p;
  for (const auto& raw : cfg.cors_origins) {
    std::string_view o = trim(raw);
    while (!o.empty() && o.back() == '/') o.remove_suffix(1);
    if (auto n = normalize_origin(o)) {
      if (std::find(p.allowed_origins.begin(), p.allowed_origins.end(), *n) == p.allowed_origins.end())
        p.allowed_origins.push_back(std::move(*n));
    }
  }
  return p;
}

bool origin_allowed(const CorsPolicy& policy, std::string_view origin) {
  auto n = normalize_origin(origin);
  if (!n) return false;
  for (const auto& allowed : policy.allowed_origins) {
    // Entries built by cors_policy_from are canonical; tolerate hand-built policies too.
    if (allowed == *n) return true;
    if (auto a = normalize_origin(allowed); a && *a == *n) return true;
  }
  return false;
}

namespace {

std::string* find_header_value(Response& res, std::string_view name) {
  for (auto& [k, v] : res.headers)
    if (iequals(k, name)) return &v;
  return nullptr;
}

// Adds `token` to a comma-separated header (case-insensitive token match), creating it if needed.
void add_vary(Response& res, std::string_view token) {
  if (std::string* v = find_header_value(res, "Vary")) {
    for (const auto& part : split(*v, ','))
      if (iequals(trim(part), token) || trim(part) == "*") return;
    if (!trim(*v).empty()) v->append(", ");
    v->append(token);
    return;
  }
  res.headers.emplace_back("Vary", std::string(token));
}

}  // namespace

void apply_cors(const CorsPolicy& policy, std::optional<std::string_view> origin, Response& res) {
  add_vary(res, "Origin");
  if (!origin || !origin_allowed(policy, *origin)) return;
  if (!res.find_header("Access-Control-Allow-Origin"))
    res.headers.emplace_back("Access-Control-Allow-Origin", std::string(trim(*origin)));
  if (!res.find_header("Access-Control-Expose-Headers") && !policy.expose_headers.empty())
    res.headers.emplace_back("Access-Control-Expose-Headers", policy.expose_headers);
}

Response preflight(const CorsPolicy& policy, std::optional<std::string_view> origin,
                   std::optional<std::string_view> request_method,
                   std::optional<std::string_view> request_headers) {
  // request_headers are never echoed: the browser compares them with allow_headers itself.
  if (!origin || !origin_allowed(policy, *origin)) {
    Response r = Response::error(403, "forbidden", "不允许的跨域来源");
    add_vary(r, "Origin");
    return r;
  }
  Response r = Response::no_content();
  r.headers.emplace_back("Access-Control-Allow-Origin", std::string(trim(*origin)));
  r.headers.emplace_back("Access-Control-Allow-Methods", policy.allow_methods);
  r.headers.emplace_back("Access-Control-Allow-Headers", policy.allow_headers);
  r.headers.emplace_back("Access-Control-Max-Age", std::to_string(policy.max_age_sec));
  // Preflight answers depend on the requested method/headers as well (caches must not mix them).
  add_vary(r, "Origin");
  if (request_method) add_vary(r, "Access-Control-Request-Method");
  if (request_headers) add_vary(r, "Access-Control-Request-Headers");
  return r;
}

}  // namespace azm::http
