// Owner: WP-D
// WP0 stub: auth endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// POST /api/auth/login
http::Response auth_login(http::Ctx&) { throw NotImplemented("api::auth_login"); }

// POST /api/auth/logout
http::Response auth_logout(http::Ctx&) { throw NotImplemented("api::auth_logout"); }

// GET  /api/auth/me
http::Response auth_me(http::Ctx&) { throw NotImplemented("api::auth_me"); }

// POST /api/auth/password
http::Response auth_change_password(http::Ctx&) { throw NotImplemented("api::auth_change_password"); }

}  // namespace azm::api
