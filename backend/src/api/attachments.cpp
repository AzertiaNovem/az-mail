// Owner: WP-D
// WP0 stub: attachments endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// POST /api/attachments (file body, net)
http::Response attachments_upload(http::Ctx&) { throw NotImplemented("api::attachments_upload"); }

// GET  /api/files/:id (signed, net)
http::Response files_get(http::Ctx&) { throw NotImplemented("api::files_get"); }

// GET  /api/files/raw/:messageId (signed, net)
http::Response files_raw(http::Ctx&) { throw NotImplemented("api::files_raw"); }

}  // namespace azm::api
