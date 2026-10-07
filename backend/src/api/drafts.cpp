// Owner: WP-D
// WP0 stub: drafts endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// POST   /api/drafts
http::Response drafts_create(http::Ctx&) { throw NotImplemented("api::drafts_create"); }

// GET    /api/drafts/:id
http::Response drafts_get(http::Ctx&) { throw NotImplemented("api::drafts_get"); }

// PUT    /api/drafts/:id
http::Response drafts_update(http::Ctx&) { throw NotImplemented("api::drafts_update"); }

// DELETE /api/drafts/:id
http::Response drafts_delete(http::Ctx&) { throw NotImplemented("api::drafts_delete"); }

// POST   /api/drafts/:id/send
http::Response drafts_send(http::Ctx&) { throw NotImplemented("api::drafts_send"); }

}  // namespace azm::api
