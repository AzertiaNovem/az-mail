// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "ws/ws_session.hpp"

#include "core/errors.hpp"

namespace azm::ws {

boost::asio::awaitable<void> run_ws_session(
    boost::beast::tcp_stream, boost::beast::http::request<boost::beast::http::empty_body>,
    WsDeps&) {
  throw NotImplemented("ws::run_ws_session");
  co_return;
}

ClientMessage parse_client_message(std::string_view) {
  throw NotImplemented("ws::parse_client_message");
}

}  // namespace azm::ws
