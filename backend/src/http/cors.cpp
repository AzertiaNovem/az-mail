// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "http/cors.hpp"

#include "config.hpp"

namespace azm::http {

CorsPolicy cors_policy_from(const Config&) { throw NotImplemented("http::cors_policy_from"); }

bool origin_allowed(const CorsPolicy&, std::string_view) {
  throw NotImplemented("http::origin_allowed");
}

void apply_cors(const CorsPolicy&, std::optional<std::string_view>, Response&) {
  throw NotImplemented("http::apply_cors");
}

Response preflight(const CorsPolicy&, std::optional<std::string_view>,
                   std::optional<std::string_view>, std::optional<std::string_view>) {
  throw NotImplemented("http::preflight");
}

}  // namespace azm::http
