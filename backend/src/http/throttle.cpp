// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements.
#include "http/throttle.hpp"

#include "config.hpp"
#include "core/errors.hpp"

namespace azm::http {

struct LoginThrottle::Impl {};

LoginThrottle::LoginThrottle(Limits, const Clock&) : impl_(std::make_unique<Impl>()) {}
LoginThrottle::LoginThrottle(const Config&, const Clock&) : impl_(std::make_unique<Impl>()) {}
LoginThrottle::~LoginThrottle() = default;

std::optional<int> LoginThrottle::check(std::string_view, std::string_view) const {
  throw NotImplemented("http::LoginThrottle::check");
}
void LoginThrottle::record_failure(std::string_view, std::string_view) {
  throw NotImplemented("http::LoginThrottle::record_failure");
}
void LoginThrottle::record_success(std::string_view, std::string_view) {
  throw NotImplemented("http::LoginThrottle::record_success");
}

LoginThrottle::ScryptPermit::~ScryptPermit() {
  if (owner_) owner_->release_scrypt();
}

LoginThrottle::ScryptPermit LoginThrottle::acquire_scrypt() {
  throw NotImplemented("http::LoginThrottle::acquire_scrypt");
}

void LoginThrottle::prune() { throw NotImplemented("http::LoginThrottle::prune"); }

void LoginThrottle::release_scrypt() noexcept {}

}  // namespace azm::http
