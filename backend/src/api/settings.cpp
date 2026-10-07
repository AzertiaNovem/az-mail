// Owner: WP-D
// WP0 stub: settings endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// GET /api/settings
http::Response settings_get(http::Ctx&) { throw NotImplemented("api::settings_get"); }

// PUT /api/settings
http::Response settings_update(http::Ctx&) { throw NotImplemented("api::settings_update"); }

// GET /api/identities
http::Response identities_list(http::Ctx&) { throw NotImplemented("api::identities_list"); }

}  // namespace azm::api
