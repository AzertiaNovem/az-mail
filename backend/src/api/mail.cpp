// Owner: WP-D
// WP0 stub: mail endpoints; compiles and links, WP-D implements (see docs/CONTRACTS.md §B).
#include "api/handlers.hpp"

#include "core/errors.hpp"

namespace azm::api {

// GET  /api/threads
http::Response threads_list(http::Ctx&) { throw NotImplemented("api::threads_list"); }

// GET  /api/threads/:id
http::Response threads_get(http::Ctx&) { throw NotImplemented("api::threads_get"); }

// POST /api/threads/actions
http::Response threads_actions(http::Ctx&) { throw NotImplemented("api::threads_actions"); }

// GET  /api/messages/:id
http::Response messages_get(http::Ctx&) { throw NotImplemented("api::messages_get"); }

// PATCH /api/messages/:id
http::Response messages_patch(http::Ctx&) { throw NotImplemented("api::messages_patch"); }

// GET  /api/messages/:id/events
http::Response messages_events(http::Ctx&) { throw NotImplemented("api::messages_events"); }

// GET  /api/messages/:id/raw (net)
http::Response messages_raw(http::Ctx&) { throw NotImplemented("api::messages_raw"); }

// POST /api/messages/:id/undo-send
http::Response messages_undo_send(http::Ctx&) { throw NotImplemented("api::messages_undo_send"); }

// POST /api/messages/:id/cancel-schedule (net)
http::Response messages_cancel_schedule(http::Ctx&) { throw NotImplemented("api::messages_cancel_schedule"); }

// POST /api/messages/:id/reschedule (net)
http::Response messages_reschedule(http::Ctx&) { throw NotImplemented("api::messages_reschedule"); }

// POST /api/messages/:id/retry
http::Response messages_retry(http::Ctx&) { throw NotImplemented("api::messages_retry"); }

// GET  /api/counts
http::Response counts_get(http::Ctx&) { throw NotImplemented("api::counts_get"); }

// GET  /api/contacts
http::Response contacts_search(http::Ctx&) { throw NotImplemented("api::contacts_search"); }

}  // namespace azm::api
