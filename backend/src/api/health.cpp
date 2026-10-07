// Owner: WP-D
// GET /api/health: liveness + a trivial DB round trip (no auth, no secrets).
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "core/log.hpp"
#include "db/sqlite.hpp"
#include "services.hpp"

#include <exception>

#ifndef AZMAIL_VERSION
#define AZMAIL_VERSION "0.0.0"
#endif

namespace azm::api {

// GET /api/health → {status, version, db, time}; 503 with db:"error" when SQLite fails.
http::Response health_get(http::Ctx& ctx) {
  bool db_ok = false;
  try {
    db_ok = ctx.svc.db.read([](db::Conn& c) { return c.scalar<int64_t>("SELECT 1"); }) ==
            std::optional<int64_t>(1);
  } catch (const std::exception& e) {
    log::error("health check: database unavailable", {{"error", e.what()}});
  }
  const int64_t now = ctx.svc.now_ms();
  if (!db_ok) return http::Response::json(health_json("error", AZMAIL_VERSION, "error", now), 503);
  return http::Response::json(health_json("ok", AZMAIL_VERSION, "ok", now));
}

}  // namespace azm::api
