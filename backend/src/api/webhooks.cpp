// Owner: WP-D
// WP0 stub: webhooks endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// POST /api/webhooks/resend (svix, raw body)
http::Response webhooks_resend(http::Ctx&) { throw NotImplemented("api::webhooks_resend"); }

}  // namespace azm::api
