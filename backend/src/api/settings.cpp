// Owner: WP-D
// Settings and identities endpoints (docs/CONTRACTS.md §B).
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "db/sqlite.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

namespace azm::api {

// GET /api/settings
http::Response settings_get(http::Ctx& ctx) {
  const int64_t uid = ctx.user().user_id;
  const auto s = ctx.svc.db.read([&](db::Conn& c) { return repo::get_settings(c, uid); });
  return http::Response::json(to_json(s));
}

// PUT /api/settings → the full Settings after the partial update (emits settings.changed).
http::Response settings_update(http::Ctx& ctx) {
  const int64_t uid = ctx.user().user_id;
  const repo::SettingsPatch patch = parse_settings_patch(ctx.body_object());
  const int64_t now = ctx.svc.now_ms();
  const auto s = ctx.svc.db.write([&](db::Tx& tx) { return repo::update_settings(tx, uid, patch, now); });
  return http::Response::json(to_json(s));
}

// GET /api/identities → Identity[]
http::Response identities_list(http::Ctx& ctx) {
  const int64_t uid = ctx.user().user_id;
  const auto ids = ctx.svc.db.read([&](db::Conn& c) { return repo::identities_for_user(c, uid); });
  return http::Response::json(to_json_array<repo::Identity>(ids));
}

}  // namespace azm::api
