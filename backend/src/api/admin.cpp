// Owner: WP-D
// WP0 stub: admin endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// GET    /api/admin/users
http::Response admin_users_list(http::Ctx&) { throw NotImplemented("api::admin_users_list"); }

// POST   /api/admin/users
http::Response admin_users_create(http::Ctx&) { throw NotImplemented("api::admin_users_create"); }

// PATCH  /api/admin/users/:id
http::Response admin_users_update(http::Ctx&) { throw NotImplemented("api::admin_users_update"); }

// DELETE /api/admin/users/:id
http::Response admin_users_delete(http::Ctx&) { throw NotImplemented("api::admin_users_delete"); }

// GET    /api/admin/aliases
http::Response admin_aliases_list(http::Ctx&) { throw NotImplemented("api::admin_aliases_list"); }

// POST   /api/admin/aliases
http::Response admin_aliases_create(http::Ctx&) { throw NotImplemented("api::admin_aliases_create"); }

// PATCH  /api/admin/aliases/:id
http::Response admin_aliases_update(http::Ctx&) { throw NotImplemented("api::admin_aliases_update"); }

// DELETE /api/admin/aliases/:id
http::Response admin_aliases_delete(http::Ctx&) { throw NotImplemented("api::admin_aliases_delete"); }

// GET    /api/admin/domains
http::Response admin_domains_list(http::Ctx&) { throw NotImplemented("api::admin_domains_list"); }

// POST   /api/admin/domains
http::Response admin_domains_create(http::Ctx&) { throw NotImplemented("api::admin_domains_create"); }

// GET    /api/admin/domains/:id
http::Response admin_domains_get(http::Ctx&) { throw NotImplemented("api::admin_domains_get"); }

// DELETE /api/admin/domains/:id
http::Response admin_domains_delete(http::Ctx&) { throw NotImplemented("api::admin_domains_delete"); }

// GET    /api/admin/domains/:id/status (net)
http::Response admin_domains_status(http::Ctx&) { throw NotImplemented("api::admin_domains_status"); }

// GET    /api/admin/events
http::Response admin_events_list(http::Ctx&) { throw NotImplemented("api::admin_events_list"); }

// GET    /api/admin/inbound
http::Response admin_inbound_list(http::Ctx&) { throw NotImplemented("api::admin_inbound_list"); }

// GET    /api/admin/outbox
http::Response admin_outbox_list(http::Ctx&) { throw NotImplemented("api::admin_outbox_list"); }

// POST   /api/admin/outbox/:id/retry
http::Response admin_outbox_retry(http::Ctx&) { throw NotImplemented("api::admin_outbox_retry"); }

// GET    /api/admin/jobs
http::Response admin_jobs_list(http::Ctx&) { throw NotImplemented("api::admin_jobs_list"); }

// POST   /api/admin/jobs/:id/retry
http::Response admin_jobs_retry(http::Ctx&) { throw NotImplemented("api::admin_jobs_retry"); }

// POST   /api/admin/sync
http::Response admin_sync(http::Ctx&) { throw NotImplemented("api::admin_sync"); }

// GET    /api/admin/stats
http::Response admin_stats_get(http::Ctx&) { throw NotImplemented("api::admin_stats_get"); }

}  // namespace azm::api
