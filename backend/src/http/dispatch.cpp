// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "http/dispatch.hpp"

#include "services.hpp"

namespace azm::http {

Response dispatch(const Route&, const Request&, Params, Services&) noexcept {
  return Response::error(500, "internal_error", "服务器内部错误 (dispatch not implemented)");
}

std::optional<std::string> bearer_token(const Request&) {
  throw NotImplemented("http::bearer_token");
}

std::optional<Principal> authenticate(Services&, std::string_view) {
  throw NotImplemented("http::authenticate");
}

}  // namespace azm::http
