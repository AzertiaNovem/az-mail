// Owner: WP-D
// POST /api/webhooks/resend: Svix verification over the exact raw body, then the event is stored
// and processed in ONE transaction by jobs::process_webhook (no network; inbound fetches are
// enqueued as jobs).
#include "api/handlers.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "db/sqlite.hpp"
#include "jobs/webhook_dispatch.hpp"
#include "resend/svix.hpp"
#include "resend/types.hpp"
#include "services.hpp"

#include <boost/json/object.hpp>

#include <stdexcept>

namespace azm::api {

namespace {

[[noreturn]] void invalid_signature() {
  throw ApiError::unauthorized("invalid_signature", "Webhook 签名无效或已过期");
}

std::string_view header_or_empty(const http::Request& req, std::string_view name) {
  return req.header(name).value_or(std::string_view());
}

}  // namespace

// POST /api/webhooks/resend → 200 {ok:true}; 401 invalid_signature; 400 invalid_json.
http::Response webhooks_resend(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const std::string& secret = svc.cfg.resend_webhook_secret;
  const std::string_view id = header_or_empty(ctx.req, "svix-id");
  const std::string_view ts = header_or_empty(ctx.req, "svix-timestamp");
  const std::string_view sig = header_or_empty(ctx.req, "svix-signature");

  if (secret.empty()) {
    // Without a secret nothing can be verified: never accept unauthenticated events.
    log::warn("webhook rejected: RESEND_WEBHOOK_SECRET is not configured");
    invalid_signature();
  }
  if (id.empty() || ts.empty() || sig.empty()) invalid_signature();

  const int64_t now = svc.now_ms();
  const auto res = resend::verify_svix(secret, id, ts, sig, ctx.req.body, now / 1000,
                                       svc.cfg.webhook_tolerance_sec);
  if (res != resend::SvixResult::Ok) {
    log::warn("webhook rejected", {{"reason", resend::to_string(res)}});
    invalid_signature();
  }

  resend::WebhookEnvelope env;
  try {
    env = resend::parse_webhook(ctx.req.body);
  } catch (const std::invalid_argument&) {
    throw ApiError::bad_request("invalid_json", "Webhook 内容格式无效");
  }

  const auto outcome = svc.db.write(
      [&](db::Tx& tx) { return jobs::process_webhook(tx, env, id, ctx.req.body, now); });
  log::debug("webhook processed", {{"type", env.type}, {"result", jobs::to_string(outcome.result)}});
  boost::json::object ok;
  ok["ok"] = true;
  return http::Response::json(ok);
}

}  // namespace azm::api
