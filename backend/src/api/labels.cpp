// Owner: WP-D
// Label endpoints (docs/CONTRACTS.md §B). Per-owner: every repo call takes the caller's id.
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "core/errors.hpp"
#include "db/sqlite.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

namespace azm::api {

// GET    /api/labels → Label[]
http::Response labels_list(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const auto labels = ctx.svc.db.read([&](db::Conn& c) { return repo::list_labels(c, owner); });
  return http::Response::json(to_json_array<repo::Label>(labels));
}

// POST   /api/labels → 201 Label
http::Response labels_create(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const repo::LabelInput in = parse_label_input(ctx.body_object());
  const int64_t now = ctx.svc.now_ms();
  const auto l = ctx.svc.db.write([&](db::Tx& tx) { return repo::create_label(tx, owner, in, now); });
  return http::Response::json(to_json(l), 201);
}

// GET    /api/labels/:id
http::Response labels_get(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto l = ctx.svc.db.read([&](db::Conn& c) { return repo::get_label(c, owner, id); });
  if (!l) throw ApiError::not_found("not_found", "标签不存在");
  return http::Response::json(to_json(*l));
}

// PATCH  /api/labels/:id
http::Response labels_update(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const repo::LabelPatch patch = parse_label_patch(ctx.body_object());
  const auto l = ctx.svc.db.write([&](db::Tx& tx) { return repo::update_label(tx, owner, id, patch); });
  return http::Response::json(to_json(l));
}

// DELETE /api/labels/:id → 204
http::Response labels_delete(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  ctx.svc.db.write([&](db::Tx& tx) { repo::delete_label(tx, owner, id); });
  return http::Response::no_content();
}

}  // namespace azm::api
