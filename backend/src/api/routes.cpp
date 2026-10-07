// Owner: WP-D
// The route table is written in WP0 as part of the contract (docs/CONTRACTS.md §B); handlers
// are stubs until WP-D implements them. register_routes depends on WP-A's Router::add.
#include "api/routes.hpp"

#include "api/handlers.hpp"
#include "config.hpp"

namespace azm::api {

namespace {
using http::AuthReq;
using http::BodyMode;
using http::Exec;
using V = boost::beast::http::verb;
}  // namespace

RouteLimits route_limits_from(const Config& cfg) {
  RouteLimits l;
  l.json = cfg.json_body_limit;
  l.draft = cfg.draft_body_limit;
  l.upload = cfg.upload_body_limit;
  l.webhook = cfg.webhook_body_limit;
  return l;
}

std::vector<http::Route> route_table(const RouteLimits& L) {
  const std::size_t J = L.json, D = L.draft, U = L.upload, W = L.webhook, N = L.bodyless;
  return {
      // ---- auth / settings ---------------------------------------------------------------
      {V::post, "/api/auth/login", AuthReq::None, BodyMode::Json, J, Exec::Db, auth_login},
      {V::post, "/api/auth/logout", AuthReq::User, BodyMode::None, N, Exec::Db, auth_logout},
      {V::get, "/api/auth/me", AuthReq::User, BodyMode::None, 0, Exec::Db, auth_me},
      {V::post, "/api/auth/password", AuthReq::User, BodyMode::Json, J, Exec::Db, auth_change_password},
      {V::get, "/api/settings", AuthReq::User, BodyMode::None, 0, Exec::Db, settings_get},
      {V::put, "/api/settings", AuthReq::User, BodyMode::Json, J, Exec::Db, settings_update},
      {V::get, "/api/identities", AuthReq::User, BodyMode::None, 0, Exec::Db, identities_list},

      // ---- threads / messages ------------------------------------------------------------
      {V::get, "/api/threads", AuthReq::User, BodyMode::None, 0, Exec::Db, threads_list},
      {V::post, "/api/threads/actions", AuthReq::User, BodyMode::Json, J, Exec::Db, threads_actions},
      {V::get, "/api/threads/:id", AuthReq::User, BodyMode::None, 0, Exec::Db, threads_get},
      {V::get, "/api/messages/:id", AuthReq::User, BodyMode::None, 0, Exec::Db, messages_get},
      {V::patch, "/api/messages/:id", AuthReq::User, BodyMode::Json, J, Exec::Db, messages_patch},
      {V::get, "/api/messages/:id/events", AuthReq::User, BodyMode::None, 0, Exec::Db, messages_events},
      {V::get, "/api/messages/:id/raw", AuthReq::User, BodyMode::None, 0, Exec::Files, messages_raw},
      {V::post, "/api/messages/:id/undo-send", AuthReq::User, BodyMode::None, N, Exec::Db,
       messages_undo_send},
      {V::post, "/api/messages/:id/cancel-schedule", AuthReq::User, BodyMode::None, N, Exec::Net,
       messages_cancel_schedule},
      {V::post, "/api/messages/:id/reschedule", AuthReq::User, BodyMode::Json, J, Exec::Net,
       messages_reschedule},
      {V::post, "/api/messages/:id/retry", AuthReq::User, BodyMode::None, N, Exec::Db, messages_retry},
      {V::get, "/api/counts", AuthReq::User, BodyMode::None, 0, Exec::Db, counts_get},
      {V::get, "/api/contacts", AuthReq::User, BodyMode::None, 0, Exec::Db, contacts_search},

      // ---- drafts ------------------------------------------------------------------------
      {V::post, "/api/drafts", AuthReq::User, BodyMode::Json, D, Exec::Db, drafts_create},
      {V::get, "/api/drafts/:id", AuthReq::User, BodyMode::None, 0, Exec::Db, drafts_get},
      {V::put, "/api/drafts/:id", AuthReq::User, BodyMode::Json, D, Exec::Db, drafts_update},
      {V::delete_, "/api/drafts/:id", AuthReq::User, BodyMode::None, N, Exec::Db, drafts_delete},
      {V::post, "/api/drafts/:id/send", AuthReq::User, BodyMode::Json, D, Exec::Db, drafts_send},

      // ---- attachments / files -----------------------------------------------------------
      {V::post, "/api/attachments", AuthReq::User, BodyMode::File, U, Exec::Files, attachments_upload},
      {V::get, "/api/files/raw/:messageId", AuthReq::Signed, BodyMode::None, 0, Exec::Files, files_raw},
      {V::get, "/api/files/:id", AuthReq::Signed, BodyMode::None, 0, Exec::Files, files_get},

      // ---- labels / counts ---------------------------------------------------------------
      {V::get, "/api/labels", AuthReq::User, BodyMode::None, 0, Exec::Db, labels_list},
      {V::post, "/api/labels", AuthReq::User, BodyMode::Json, J, Exec::Db, labels_create},
      {V::get, "/api/labels/:id", AuthReq::User, BodyMode::None, 0, Exec::Db, labels_get},
      {V::patch, "/api/labels/:id", AuthReq::User, BodyMode::Json, J, Exec::Db, labels_update},
      {V::delete_, "/api/labels/:id", AuthReq::User, BodyMode::None, N, Exec::Db, labels_delete},

      // ---- webhooks / health -------------------------------------------------------------
      {V::post, "/api/webhooks/resend", AuthReq::Webhook, BodyMode::Raw, W, Exec::Db, webhooks_resend},
      {V::get, "/api/health", AuthReq::None, BodyMode::None, 0, Exec::Db, health_get},

      // ---- admin -------------------------------------------------------------------------
      {V::get, "/api/admin/users", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_users_list},
      {V::post, "/api/admin/users", AuthReq::Admin, BodyMode::Json, J, Exec::Db, admin_users_create},
      {V::patch, "/api/admin/users/:id", AuthReq::Admin, BodyMode::Json, J, Exec::Db, admin_users_update},
      {V::delete_, "/api/admin/users/:id", AuthReq::Admin, BodyMode::None, N, Exec::Db,
       admin_users_delete},
      {V::get, "/api/admin/aliases", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_aliases_list},
      {V::post, "/api/admin/aliases", AuthReq::Admin, BodyMode::Json, J, Exec::Db, admin_aliases_create},
      {V::patch, "/api/admin/aliases/:id", AuthReq::Admin, BodyMode::Json, J, Exec::Db,
       admin_aliases_update},
      {V::delete_, "/api/admin/aliases/:id", AuthReq::Admin, BodyMode::None, N, Exec::Db,
       admin_aliases_delete},
      {V::get, "/api/admin/domains", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_domains_list},
      {V::post, "/api/admin/domains", AuthReq::Admin, BodyMode::Json, J, Exec::Db, admin_domains_create},
      {V::get, "/api/admin/domains/:id", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_domains_get},
      {V::delete_, "/api/admin/domains/:id", AuthReq::Admin, BodyMode::None, N, Exec::Db,
       admin_domains_delete},
      {V::get, "/api/admin/domains/:id/status", AuthReq::Admin, BodyMode::None, 0, Exec::Net,
       admin_domains_status},
      {V::get, "/api/admin/events", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_events_list},
      {V::get, "/api/admin/inbound", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_inbound_list},
      {V::get, "/api/admin/outbox", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_outbox_list},
      {V::post, "/api/admin/outbox/:id/retry", AuthReq::Admin, BodyMode::None, N, Exec::Db,
       admin_outbox_retry},
      {V::get, "/api/admin/jobs", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_jobs_list},
      {V::post, "/api/admin/jobs/:id/retry", AuthReq::Admin, BodyMode::None, N, Exec::Db,
       admin_jobs_retry},
      {V::post, "/api/admin/sync", AuthReq::Admin, BodyMode::None, N, Exec::Db, admin_sync},
      {V::get, "/api/admin/stats", AuthReq::Admin, BodyMode::None, 0, Exec::Db, admin_stats_get},
  };
}

void register_routes(http::Router& router) {
  for (auto& r : route_table(RouteLimits{})) router.add(std::move(r));
}

void register_routes(http::Router& router, const Config& cfg) {
  for (auto& r : route_table(route_limits_from(cfg))) router.add(std::move(r));
}

}  // namespace azm::api
