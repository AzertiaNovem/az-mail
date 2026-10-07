// Owner: WP-D
// Draft endpoints (docs/CONTRACTS.md §B): parse with mail/serde, one transaction each, the
// caller's id as owner. Signed URLs in responses come from svc.signed_urls.
#include "api/handlers.hpp"
#include "core/errors.hpp"
#include "db/sqlite.hpp"
#include "mail/drafts.hpp"
#include "mail/serde.hpp"
#include "services.hpp"

namespace azm::api {

// POST   /api/drafts → 201 Draft
http::Response drafts_create(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const mail::DraftInput in = mail::parse_draft_input(ctx.body_object());
  const auto d = ctx.svc.db.write(
      [&](db::Tx& tx) { return mail::create_draft(tx, ctx.svc.signed_urls, owner, in); });
  return http::Response::json(mail::to_json(d), 201);
}

// GET    /api/drafts/:id
http::Response drafts_get(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto d =
      ctx.svc.db.read([&](db::Conn& c) { return mail::get_draft(c, ctx.svc.signed_urls, owner, id); });
  if (!d) throw ApiError::not_found("not_found", "草稿不存在");
  return http::Response::json(mail::to_json(*d));
}

// PUT    /api/drafts/:id → Draft (409 version_conflict {current})
http::Response drafts_update(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const mail::DraftUpdate up = mail::parse_draft_update(ctx.body_object());
  const auto d = ctx.svc.db.write([&](db::Tx& tx) {
    return mail::update_draft(tx, ctx.svc.signed_urls, owner, id, up.version, up.input, up.force);
  });
  return http::Response::json(mail::to_json(d));
}

// DELETE /api/drafts/:id → 204
http::Response drafts_delete(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  ctx.svc.db.write([&](db::Tx& tx) { mail::delete_draft(tx, owner, id); });
  return http::Response::no_content();
}

// POST   /api/drafts/:id/send → 202 SendResult. Uses the queue_send overload with SignedUrls so a
// 409 version_conflict carries details.current as a full Draft (CONTRACTS §H 25).
http::Response drafts_send(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  mail::SendOptions opts = mail::parse_send_options(ctx.body_object());
  opts.now_ms = ctx.svc.now_ms();
  const auto r = ctx.svc.db.write([&](db::Tx& tx) {
    return mail::queue_send(tx, ctx.svc.cfg, ctx.svc.signed_urls, owner, id, opts);
  });
  return http::Response::json(mail::to_json(r), 202);
}

}  // namespace azm::api
