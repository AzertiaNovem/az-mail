// Owner: WP-C
// WP0 stub: to_string is trivial and implemented; the parsers are WP-C's.
#include "resend/types.hpp"

#include "core/errors.hpp"

namespace azm::resend {

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

Error classify_error(int, std::string_view, std::optional<std::string_view>) {
  throw NotImplemented("resend::classify_error");
}

WebhookEnvelope parse_webhook(std::string_view) { throw NotImplemented("resend::parse_webhook"); }

}  // namespace azm::resend
