// Owner: WP-C
// Resend webhook processing inside the request's write transaction (DESIGN B3/B4/B9/B10).
#include "jobs/webhook_dispatch.hpp"

#include "db/kv.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "mail/inbound.hpp"
#include "mail/outbound.hpp"
#include "mail/types.hpp"


#include <stdexcept>

namespace azm::jobs {
namespace {

// Event data keys that are mail content or addressing: kept out of delivery_events.detail_json.
bool content_key(std::string_view k) {
  return k == "to" || k == "cc" || k == "bcc" || k == "from" || k == "reply_to" || k == "subject" ||
         k == "headers" || k == "html" || k == "text" || k == "attachments" || k == "tags";
}

WebhookOutcome route(db::Tx& tx, const resend::WebhookEnvelope& env, std::string_view svix_id, int64_t now) {
  if (env.is_received()) {
    if (!env.email_id) return {WebhookResult::Error, "missing_email_id"};
    const std::string& id = *env.email_id;
    mail::record_inbound_pending(tx, id, mail::InboundSource::Webhook, now);
    enqueue(tx, kinds::kInboundFetch,
            {{std::string(payload::kResendId), id}, {std::string(payload::kSource), "webhook"}},
            {.dedupe_key = dedupe_inbound_fetch(id), .priority = kPriorityNormal, .now_ms = now});
    return {WebhookResult::Enqueued, {}};
  }
  if (env.is_outbound_event()) {
    mail::OutboundEvent ev;
    ev.resend_id = env.email_id;
    if (auto it = env.tags.find(std::string(mail::kTagOutbound)); it != env.tags.end() && !it->second.empty())
      ev.uuid = it->second;
    if (!ev.resend_id && !ev.uuid) return {WebhookResult::Error, "missing_email_id"};
    ev.type = env.type;
    ev.occurred_at = env.created_at_ms > 0 ? env.created_at_ms : now;
    ev.recipients = env.to;
    for (const auto& [k, v] : env.data)
      if (!content_key(k)) ev.detail[k] = v;  // bounce / failed / delivery_delayed details, ids, times
    ev.source_key = std::string(svix_id);
    if (env.message_id) {
      std::string mid = mail::normalize_message_id(*env.message_id);
      if (!mid.empty()) ev.message_id = std::move(mid);
    }
    const auto r = mail::apply_outbound_event(tx, ev);
    if (r.outcome == mail::EventOutcome::Unknown) return {WebhookResult::IgnoredUnknown, {}};  // B9
    return {WebhookResult::Applied, {}};
  }
  return {WebhookResult::IgnoredUnknown, {}};
}

}  // namespace

std::string_view to_string(WebhookResult r) {
  switch (r) {
    case WebhookResult::Applied: return "applied";
    case WebhookResult::Enqueued: return "enqueued";
    case WebhookResult::IgnoredUnknown: return "ignored_unknown";
    case WebhookResult::Duplicate: return "duplicate";
    case WebhookResult::Error: return "error";
  }
  return "error";
}

WebhookOutcome process_webhook(db::Tx& tx, const resend::WebhookEnvelope& env, std::string_view svix_id,
                               std::string_view raw_body, int64_t now_ms) {
  tx.run(
      "INSERT OR IGNORE INTO webhook_events(svix_id, type, resend_email_id, payload, received_at) "
      "VALUES(?,?,?,?,?)",
      svix_id, env.type, env.email_id, raw_body, now_ms);
  if (tx.changes() == 0) return {WebhookResult::Duplicate, {}};  // Svix redelivery: already handled
  db::kv_set_i64(tx, db::kv_keys::kLastWebhookAt, now_ms, now_ms);

  WebhookOutcome out;
  try {
    out = route(tx, env, svix_id, now_ms);
  } catch (const std::invalid_argument&) {
    out = {WebhookResult::Error, "invalid_payload"};  // malformed content: recorded, never retried
  } catch (const std::out_of_range&) {
    out = {WebhookResult::Error, "invalid_payload"};
  }
  const std::string result =
      out.result == WebhookResult::Error ? "error:" + out.detail : std::string(to_string(out.result));
  tx.run("UPDATE webhook_events SET processed_at=?, result=? WHERE svix_id=?", now_ms, result, svix_id);
  return out;
}

}  // namespace azm::jobs
