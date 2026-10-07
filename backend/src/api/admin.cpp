// Owner: WP-D
// Admin endpoints (docs/CONTRACTS.md §B). Metadata only: admins never see mail bodies (D7).
// Every mutation is audited in the same transaction; WS revocations happen after COMMIT.
#include "api/common.hpp"
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "core/address.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "mail/outbound.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "services.hpp"

#include <boost/json/object.hpp>

namespace azm::api {

namespace json = boost::json;

namespace {

constexpr int kAdminListLimit = 200;
constexpr int kEventsPageSize = 50;

[[noreturn]] void user_not_found() { throw ApiError::not_found("not_found", "用户不存在"); }

repo::AdminUserRow load_admin_user(Services& svc, int64_t id) {
  auto row = svc.db.read([&](db::Conn& c) { return repo::get_user_admin(c, id); });
  if (!row) user_not_found();
  return std::move(*row);
}

}  // namespace

// GET    /api/admin/users → AdminUser[]
http::Response admin_users_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto rows = ctx.svc.db.read([](db::Conn& c) { return repo::list_users_admin(c); });
  return http::Response::json(to_json_array<repo::AdminUserRow>(rows));
}

// POST   /api/admin/users → 201 AdminUser
http::Response admin_users_create(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const auto& admin = detail::require_admin(ctx);
  const AdminUserCreate in = parse_admin_user_create(ctx.body_object());
  validate_new_password(in.password);
  // Cheap pre-checks so obviously bad requests do not cost a scrypt; create_user re-checks
  // everything inside the transaction.
  const std::string email = normalize_email(in.email);
  if (!is_valid_email(email))
    throw ApiError::bad_request("invalid_field", "邮箱地址无效", detail::field_detail("email"));
  svc.db.read([&](db::Conn& c) {
    if (!repo::find_domain(c, domain_of(email)))
      throw ApiError::unprocessable("unknown_domain", "该邮箱的域名不存在，请先添加域名");
    if (repo::find_address(c, email))
      throw ApiError::conflict("address_exists", "该邮箱地址已被用户或别名占用");
  });
  const std::string hash = detail::hash_password(svc, in.password);

  const int64_t now = svc.now_ms();
  const auto user = svc.db.write([&](db::Tx& tx) {
    repo::NewUser nu;
    nu.email = in.email;
    nu.display_name = in.display_name;
    nu.password_hash = hash;
    nu.is_admin = in.is_admin;
    nu.undo_send_seconds = svc.cfg.default_undo_send_seconds;
    auto u = repo::create_user(tx, nu, now);
    repo::audit(tx, admin.user_id, "user.create", u.email, {{"user_id", u.id}, {"is_admin", u.is_admin}},
                ctx.req.remote_ip, now);
    return u;
  });
  return http::Response::json(to_json(load_admin_user(svc, user.id)), 201);
}

// PATCH  /api/admin/users/:id → AdminUser
http::Response admin_users_update(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const AdminUserPatchInput in = parse_admin_user_patch(ctx.body_object());
  std::optional<std::string> hash;
  if (in.password) {
    validate_new_password(*in.password);
    hash = detail::hash_password(svc, *in.password);  // outside the transaction
  }
  const bool disabling = in.disabled.value_or(false);
  // An admin resetting their OWN password keeps the session they are using.
  std::optional<int64_t> keep;
  if (id == admin.user_id) keep = admin.session_id;

  const int64_t now = svc.now_ms();
  const auto revoked = svc.db.write([&](db::Tx& tx) {
    std::vector<int64_t> ids;
    if (disabling) ids = repo::revoke_all_sessions(tx, id);
    else if (hash) ids = repo::revoke_all_sessions(tx, id, keep);
    repo::UserPatch p;
    p.display_name = in.display_name;
    p.is_admin = in.is_admin;
    p.disabled = in.disabled;
    p.password_hash = hash;
    const auto u = repo::update_user(tx, id, p, now);
    json::object detail;
    if (in.display_name) detail["display_name"] = true;
    if (in.is_admin) detail["is_admin"] = *in.is_admin;
    if (in.disabled) detail["disabled"] = *in.disabled;
    if (hash) detail["password_reset"] = true;
    detail["user_id"] = id;
    repo::audit(tx, admin.user_id, "user.update", u.email, detail, ctx.req.remote_ip, now);
    return ids;
  });
  if (disabling) svc.notifier.revoke_user(id);
  else
    for (int64_t sid : revoked) svc.notifier.revoke_session(sid);
  return http::Response::json(to_json(load_admin_user(svc, id)));
}

// DELETE /api/admin/users/:id → 204
http::Response admin_users_delete(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const int64_t now = svc.now_ms();
  svc.db.write([&](db::Tx& tx) {
    const auto u = repo::get_user(tx.conn(), id);
    repo::delete_user(tx, id, admin.user_id);  // 404 / cannot_delete_self / last_admin
    repo::audit(tx, admin.user_id, "user.delete", u ? u->email : std::string(), {{"user_id", id}},
                ctx.req.remote_ip, now);
  });
  svc.notifier.revoke_user(id);
  return http::Response::no_content();
}

// GET    /api/admin/aliases → AdminAlias[]
http::Response admin_aliases_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto rows = ctx.svc.db.read([](db::Conn& c) { return repo::list_aliases(c); });
  return http::Response::json(to_json_array<repo::Alias>(rows));
}

// POST   /api/admin/aliases → 201 AdminAlias
http::Response admin_aliases_create(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const repo::AliasInput in = parse_alias_input(ctx.body_object());
  const int64_t now = ctx.svc.now_ms();
  const auto a = ctx.svc.db.write([&](db::Tx& tx) {
    auto alias = repo::create_alias(tx, in, now);
    repo::audit(tx, admin.user_id, "alias.create", alias.email,
                {{"alias_id", alias.id}, {"members", static_cast<int64_t>(alias.members.size())}},
                ctx.req.remote_ip, now);
    return alias;
  });
  return http::Response::json(to_json(a), 201);
}

// PATCH  /api/admin/aliases/:id → AdminAlias (members replaced wholesale)
http::Response admin_aliases_update(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const repo::AliasPatch p = parse_alias_patch(ctx.body_object());
  const int64_t now = ctx.svc.now_ms();
  const auto a = ctx.svc.db.write([&](db::Tx& tx) {
    auto alias = repo::update_alias(tx, id, p);
    json::object detail;
    detail["alias_id"] = id;
    if (p.email) detail["email"] = true;
    if (p.display_name) detail["display_name"] = true;
    if (p.share_sent) detail["share_sent"] = *p.share_sent;
    if (p.members) detail["members"] = static_cast<int64_t>(p.members->size());
    repo::audit(tx, admin.user_id, "alias.update", alias.email, detail, ctx.req.remote_ip, now);
    return alias;
  });
  return http::Response::json(to_json(a));
}

// DELETE /api/admin/aliases/:id → 204 (409 alias_in_use)
http::Response admin_aliases_delete(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const int64_t now = ctx.svc.now_ms();
  ctx.svc.db.write([&](db::Tx& tx) {
    const auto a = repo::get_address(tx.conn(), id);
    repo::delete_alias(tx, id);
    repo::audit(tx, admin.user_id, "alias.delete", a ? a->email : std::string(), {{"alias_id", id}},
                ctx.req.remote_ip, now);
  });
  return http::Response::no_content();
}

// GET    /api/admin/domains → DomainRow[]
http::Response admin_domains_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto rows = ctx.svc.db.read([](db::Conn& c) { return repo::list_domains(c); });
  return http::Response::json(to_json_array<repo::Domain>(rows));
}

// POST   /api/admin/domains → 201 DomainRow
http::Response admin_domains_create(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const std::string name = parse_domain_input(ctx.body_object());
  const int64_t now = ctx.svc.now_ms();
  const auto d = ctx.svc.db.write([&](db::Tx& tx) {
    auto dom = repo::add_domain(tx, name, now);
    repo::audit(tx, admin.user_id, "domain.add", dom.name, {{"domain_id", dom.id}}, ctx.req.remote_ip,
                now);
    return dom;
  });
  return http::Response::json(to_json(d), 201);
}

// GET    /api/admin/domains/:id → DomainRow
http::Response admin_domains_get(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const auto d = ctx.svc.db.read([&](db::Conn& c) { return repo::get_domain(c, id); });
  if (!d) throw ApiError::not_found("not_found", "域名不存在");
  return http::Response::json(to_json(*d));
}

// DELETE /api/admin/domains/:id → 204 (409 domain_in_use)
http::Response admin_domains_delete(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const int64_t now = ctx.svc.now_ms();
  ctx.svc.db.write([&](db::Tx& tx) {
    const auto d = repo::get_domain(tx.conn(), id);
    repo::remove_domain(tx, id);
    repo::audit(tx, admin.user_id, "domain.remove", d ? d->name : std::string(), {{"domain_id", id}},
                ctx.req.remote_ip, now);
  });
  return http::Response::no_content();
}

// GET    /api/admin/domains/:id/status (net) → DomainStatus (resend:null when Resend lacks it)
http::Response admin_domains_status(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const auto d = svc.db.read([&](db::Conn& c) { return repo::get_domain(c, id); });
  if (!d) throw ApiError::not_found("not_found", "域名不存在");

  std::optional<resend::DomainInfo> info;
  try {
    resend::Client& client = svc.resend_client();
    for (const auto& r : client.list_domains()) {
      if (!iequals(r.name, d->name)) continue;
      try {
        info = client.get_domain(r.id);  // includes the DNS records
      } catch (const resend::Error& e) {
        if (e.kind != resend::Error::Kind::NotFound) throw;
        // Deleted at Resend between the two calls: report it as unknown.
      }
      break;
    }
  } catch (const resend::Error& e) {
    detail::throw_resend_error(e, "domain_status");
  }
  return http::Response::json(domain_status_json(*d, info));
}

// GET    /api/admin/events?type&cursor → {items, next_cursor}
http::Response admin_events_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto type = detail::query_nonempty(ctx, "type");
  const auto cursor = detail::query_nonempty(ctx, "cursor");
  const auto page = ctx.svc.db.read([&](db::Conn& c) {
    return repo::list_webhook_events(c, type ? std::optional<std::string_view>(*type) : std::nullopt,
                                     cursor ? std::optional<std::string_view>(*cursor) : std::nullopt,
                                     kEventsPageSize);
  });
  return http::Response::json(to_json(page));
}

// GET    /api/admin/inbound?state → InboundRow[]
http::Response admin_inbound_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto state = detail::query_nonempty(ctx, "state");
  const auto rows = ctx.svc.db.read([&](db::Conn& c) {
    return repo::list_inbound(c, state ? std::optional<std::string_view>(*state) : std::nullopt,
                              kAdminListLimit);
  });
  return http::Response::json(to_json_array<repo::InboundRow>(rows));
}

// GET    /api/admin/outbox?status → OutboxRow[]
http::Response admin_outbox_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto status = detail::query_nonempty(ctx, "status");
  const auto rows = ctx.svc.db.read([&](db::Conn& c) {
    return repo::list_outbox(c, status ? std::optional<std::string_view>(*status) : std::nullopt,
                             kAdminListLimit);
  });
  return http::Response::json(to_json_array<repo::OutboxRow>(rows));
}

// POST   /api/admin/outbox/:id/retry → the NEW OutboxRow (new id and uuid)
http::Response admin_outbox_retry(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const int64_t now = ctx.svc.now_ms();
  const int64_t new_id = ctx.svc.db.write([&](db::Tx& tx) {
    const int64_t nid = mail::admin_retry_outbound(tx, id, now);
    repo::audit(tx, admin.user_id, "outbox.retry", std::to_string(id), {{"new_outbound_id", nid}},
                ctx.req.remote_ip, now);
    return nid;
  });
  const auto row = ctx.svc.db.read([&](db::Conn& c) { return repo::get_outbox_row(c, new_id); });
  if (!row) throw ApiError::not_found("not_found", "发件记录不存在");
  return http::Response::json(to_json(*row));
}

// GET    /api/admin/jobs?state → JobRow[]
http::Response admin_jobs_list(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const auto state = detail::query_nonempty(ctx, "state");
  const auto rows = ctx.svc.db.read([&](db::Conn& c) {
    return repo::list_jobs(c, state ? std::optional<std::string_view>(*state) : std::nullopt,
                           kAdminListLimit);
  });
  return http::Response::json(to_json_array<repo::JobRow>(rows));
}

// POST   /api/admin/jobs/:id/retry → JobRow
http::Response admin_jobs_retry(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t id = ctx.id("id");
  const int64_t now = ctx.svc.now_ms();
  ctx.svc.db.write([&](db::Tx& tx) {
    const auto job = repo::get_job(tx.conn(), id);
    if (!job) throw ApiError::not_found("not_found", "任务不存在");
    if (!jobs::retry_dead(tx, id, now))
      throw ApiError::conflict("invalid_state", "只有失败或已取消的任务可以重试");
    tx.wake_jobs();
    repo::audit(tx, admin.user_id, "job.retry", job->kind, {{"job_id", id}}, ctx.req.remote_ip, now);
  });
  const auto row = ctx.svc.db.read([&](db::Conn& c) { return repo::get_job(c, id); });
  if (!row) throw ApiError::not_found("not_found", "任务不存在");
  return http::Response::json(to_json(*row));
}

// POST   /api/admin/sync → 202 {job_id}
http::Response admin_sync(http::Ctx& ctx) {
  const auto& admin = detail::require_admin(ctx);
  const int64_t now = ctx.svc.now_ms();
  const int64_t job_id = ctx.svc.db.write([&](db::Tx& tx) {
    jobs::EnqueueOpts o;
    o.dedupe_key = std::string(jobs::kDedupeManualPoll);
    o.now_ms = now;
    json::object payload;
    payload[jobs::payload::kManual] = true;
    const int64_t jid = jobs::enqueue(tx, jobs::kinds::kPollReceiving, std::move(payload), o);
    repo::audit(tx, admin.user_id, "sync.manual", "", {{"job_id", jid}}, ctx.req.remote_ip, now);
    return jid;
  });
  json::object out;
  out["job_id"] = job_id;
  return http::Response::json(out, 202);
}

// GET    /api/admin/stats → AdminStats (storage backend/delivery from the running config)
http::Response admin_stats_get(http::Ctx& ctx) {
  detail::require_admin(ctx);
  const int64_t now = ctx.svc.now_ms();
  auto st = ctx.svc.db.read([&](db::Conn& c) { return repo::admin_stats(c, now); });
  st.storage.backend = std::string(ctx.svc.blobs.kind());
  st.storage.delivery = std::string(enum_name(ctx.svc.cfg.files_delivery));
  return http::Response::json(to_json(st));
}

}  // namespace azm::api
