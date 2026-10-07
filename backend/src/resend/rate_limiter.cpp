// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "resend/rate_limiter.hpp"

#include "core/errors.hpp"

namespace azm::resend {

struct RateLimiter::Impl {};

RateLimiter::RateLimiter(Options, const Clock&) : impl_(std::make_unique<Impl>()) {}
RateLimiter::~RateLimiter() = default;

bool RateLimiter::acquire(Priority, std::stop_token) {
  throw NotImplemented("resend::RateLimiter::acquire");
}
bool RateLimiter::try_acquire(Priority) { throw NotImplemented("resend::RateLimiter::try_acquire"); }
void RateLimiter::pause_until(int64_t) { throw NotImplemented("resend::RateLimiter::pause_until"); }
int64_t RateLimiter::paused_until() const {
  throw NotImplemented("resend::RateLimiter::paused_until");
}
double RateLimiter::available() const { throw NotImplemented("resend::RateLimiter::available"); }

}  // namespace azm::resend
