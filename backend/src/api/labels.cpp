// Owner: WP-D
// WP0 stub: labels endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// GET    /api/labels
http::Response labels_list(http::Ctx&) { throw NotImplemented("api::labels_list"); }

// POST   /api/labels
http::Response labels_create(http::Ctx&) { throw NotImplemented("api::labels_create"); }

// GET    /api/labels/:id
http::Response labels_get(http::Ctx&) { throw NotImplemented("api::labels_get"); }

// PATCH  /api/labels/:id
http::Response labels_update(http::Ctx&) { throw NotImplemented("api::labels_update"); }

// DELETE /api/labels/:id
http::Response labels_delete(http::Ctx&) { throw NotImplemented("api::labels_delete"); }

}  // namespace azm::api
