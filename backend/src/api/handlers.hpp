// Owner: WP-D
//
// One function per REST endpoint (registered by api::route_table). Handlers run on a blocking
// pool via http::dispatch, which already enforced Route::auth (User/Admin principal present).
// They: parse input (core/json, mail/serde, api/dto), run reads in svc.db.read and writes in
// ONE svc.db.write per step, never do network I/O inside a transaction, and return
// http::Response. Errors are thrown as ApiError (codes per docs/CONTRACTS.md §B / §E).
// Tests call these directly with an http::Ctx built from a fake Services (test_api_handlers).
#pragma once

#include "http/types.hpp"

namespace azm::api {

// ---- auth.cpp ------------------------------------------------------------------------------
http::Response auth_login(http::Ctx& ctx);            // POST /api/auth/login
http::Response auth_logout(http::Ctx& ctx);           // POST /api/auth/logout
http::Response auth_me(http::Ctx& ctx);               // GET  /api/auth/me
http::Response auth_change_password(http::Ctx& ctx);  // POST /api/auth/password

// ---- settings.cpp --------------------------------------------------------------------------
http::Response settings_get(http::Ctx& ctx);     // GET /api/settings
http::Response settings_update(http::Ctx& ctx);  // PUT /api/settings
http::Response identities_list(http::Ctx& ctx);  // GET /api/identities

// ---- mail.cpp ------------------------------------------------------------------------------
http::Response threads_list(http::Ctx& ctx);              // GET  /api/threads
http::Response threads_get(http::Ctx& ctx);               // GET  /api/threads/:id
http::Response threads_actions(http::Ctx& ctx);           // POST /api/threads/actions
http::Response messages_get(http::Ctx& ctx);              // GET  /api/messages/:id
http::Response messages_patch(http::Ctx& ctx);            // PATCH /api/messages/:id
http::Response messages_events(http::Ctx& ctx);           // GET  /api/messages/:id/events
http::Response messages_raw(http::Ctx& ctx);              // GET  /api/messages/:id/raw (files)
http::Response messages_undo_send(http::Ctx& ctx);        // POST /api/messages/:id/undo-send
http::Response messages_cancel_schedule(http::Ctx& ctx);  // POST /api/messages/:id/cancel-schedule (net)
http::Response messages_reschedule(http::Ctx& ctx);       // POST /api/messages/:id/reschedule (net)
http::Response messages_retry(http::Ctx& ctx);            // POST /api/messages/:id/retry
http::Response counts_get(http::Ctx& ctx);                // GET  /api/counts
http::Response contacts_search(http::Ctx& ctx);           // GET  /api/contacts

// ---- drafts.cpp ----------------------------------------------------------------------------
http::Response drafts_create(http::Ctx& ctx);  // POST   /api/drafts
http::Response drafts_get(http::Ctx& ctx);     // GET    /api/drafts/:id
http::Response drafts_update(http::Ctx& ctx);  // PUT    /api/drafts/:id
http::Response drafts_delete(http::Ctx& ctx);  // DELETE /api/drafts/:id
http::Response drafts_send(http::Ctx& ctx);    // POST   /api/drafts/:id/send (queue_send with svc.signed_urls)

// ---- attachments.cpp -----------------------------------------------------------------------
// POST /api/attachments (file body, files pool): holds blob_writer_guard() (core/blob_store.hpp)
// from put_file until the mail::create_upload transaction has committed; the session deletes
// req.body_file if the handler did not consume it.
http::Response attachments_upload(http::Ctx& ctx);
http::Response files_get(http::Ctx& ctx);           // GET  /api/files/:id (signed, files)
http::Response files_raw(http::Ctx& ctx);           // GET  /api/files/raw/:messageId (signed, files)

// ---- labels.cpp ----------------------------------------------------------------------------
http::Response labels_list(http::Ctx& ctx);    // GET    /api/labels
http::Response labels_create(http::Ctx& ctx);  // POST   /api/labels
http::Response labels_get(http::Ctx& ctx);     // GET    /api/labels/:id
http::Response labels_update(http::Ctx& ctx);  // PATCH  /api/labels/:id
http::Response labels_delete(http::Ctx& ctx);  // DELETE /api/labels/:id

// ---- webhooks.cpp --------------------------------------------------------------------------
http::Response webhooks_resend(http::Ctx& ctx);  // POST /api/webhooks/resend (svix, raw body)

// ---- health.cpp ----------------------------------------------------------------------------
http::Response health_get(http::Ctx& ctx);  // GET /api/health

// ---- admin.cpp -----------------------------------------------------------------------------
http::Response admin_users_list(http::Ctx& ctx);      // GET    /api/admin/users
http::Response admin_users_create(http::Ctx& ctx);    // POST   /api/admin/users (undo = cfg.default_undo_send_seconds)
http::Response admin_users_update(http::Ctx& ctx);    // PATCH  /api/admin/users/:id
http::Response admin_users_delete(http::Ctx& ctx);    // DELETE /api/admin/users/:id
http::Response admin_aliases_list(http::Ctx& ctx);    // GET    /api/admin/aliases
http::Response admin_aliases_create(http::Ctx& ctx);  // POST   /api/admin/aliases
http::Response admin_aliases_update(http::Ctx& ctx);  // PATCH  /api/admin/aliases/:id
http::Response admin_aliases_delete(http::Ctx& ctx);  // DELETE /api/admin/aliases/:id (409 alias_in_use)
http::Response admin_domains_list(http::Ctx& ctx);    // GET    /api/admin/domains
http::Response admin_domains_create(http::Ctx& ctx);  // POST   /api/admin/domains
http::Response admin_domains_get(http::Ctx& ctx);     // GET    /api/admin/domains/:id
http::Response admin_domains_delete(http::Ctx& ctx);  // DELETE /api/admin/domains/:id
http::Response admin_domains_status(http::Ctx& ctx);  // GET    /api/admin/domains/:id/status (net)
http::Response admin_events_list(http::Ctx& ctx);     // GET    /api/admin/events
http::Response admin_inbound_list(http::Ctx& ctx);    // GET    /api/admin/inbound
http::Response admin_outbox_list(http::Ctx& ctx);     // GET    /api/admin/outbox
http::Response admin_outbox_retry(http::Ctx& ctx);    // POST   /api/admin/outbox/:id/retry → the NEW OutboxRow
http::Response admin_jobs_list(http::Ctx& ctx);       // GET    /api/admin/jobs
http::Response admin_jobs_retry(http::Ctx& ctx);      // POST   /api/admin/jobs/:id/retry
http::Response admin_sync(http::Ctx& ctx);            // POST   /api/admin/sync
http::Response admin_stats_get(http::Ctx& ctx);       // GET    /api/admin/stats

}  // namespace azm::api
