// Owner: WP-D
// WP0 stub: health endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// GET /api/health
http::Response health_get(http::Ctx&) { throw NotImplemented("api::health_get"); }

}  // namespace azm::api
