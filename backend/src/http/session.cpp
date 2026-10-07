// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "http/session.hpp"

#include "core/errors.hpp"

namespace azm::http {

boost::asio::awaitable<void> run_session(boost::asio::ip::tcp::socket, SessionShared&) {
  throw NotImplemented("http::run_session");
  co_return;
}

std::string resolve_client_ip(std::string_view, std::optional<std::string_view>,
                              std::span<const std::string>) {
  throw NotImplemented("http::resolve_client_ip");
}

std::optional<Response> precheck_body(const Route&, std::optional<std::uint64_t>) {
  throw NotImplemented("http::precheck_body");
}

void finalize_response(Response&, std::string_view, const CorsPolicy&,
                       std::optional<std::string_view>) {
  throw NotImplemented("http::finalize_response");
}

std::string make_request_id() { throw NotImplemented("http::make_request_id"); }

}  // namespace azm::http
