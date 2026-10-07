// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "http/server.hpp"

#include "core/errors.hpp"

namespace azm::http {

struct Server::Impl {};

Server::Server(boost::asio::io_context&, ServerDeps) : impl_(std::make_unique<Impl>()) {}
Server::~Server() = default;

void Server::start() { throw NotImplemented("http::Server::start"); }
void Server::stop() { throw NotImplemented("http::Server::stop"); }
uint16_t Server::port() const { throw NotImplemented("http::Server::port"); }
std::size_t Server::connections() const { throw NotImplemented("http::Server::connections"); }
std::size_t Server::inflight() const { throw NotImplemented("http::Server::inflight"); }

}  // namespace azm::http
