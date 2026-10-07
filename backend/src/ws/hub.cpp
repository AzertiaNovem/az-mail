// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements. publish/revoke are no-ops (not throws) so that
// db::Pool hooks wired to a Hub stay harmless until WP-A lands.
#include "ws/hub.hpp"

#include "core/errors.hpp"

namespace azm::ws {

struct Hub::Impl {};

Hub::Hub(std::size_t) : impl_(std::make_unique<Impl>()) {}
Hub::~Hub() = default;

void Hub::publish(int64_t, std::string, boost::json::object) {}
void Hub::revoke_session(int64_t) {}
void Hub::revoke_user(int64_t) {}

uint64_t Hub::attach(int64_t, int64_t, std::shared_ptr<WsSink>) {
  throw NotImplemented("ws::Hub::attach");
}
void Hub::detach(uint64_t) { throw NotImplemented("ws::Hub::detach"); }
void Hub::close_all() { throw NotImplemented("ws::Hub::close_all"); }
std::size_t Hub::connection_count() const { throw NotImplemented("ws::Hub::connection_count"); }
std::size_t Hub::connection_count(int64_t) const {
  throw NotImplemented("ws::Hub::connection_count");
}

}  // namespace azm::ws
