// Owner: WP-C
//
// Resend webhook processing (DESIGN §3, B3/B4/B9/B10). Pure DB work: called by
// POST /api/webhooks/resend (WP-D) AFTER Svix verification and resend::parse_webhook, inside one
// Pool::write transaction; no network I/O, no Services.
#pragma once

#include "db/sqlite.hpp"
#include "resend/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace azm::jobs {

enum class WebhookResult {
  Applied,         // outbound event recorded via mail::apply_outbound_event
  Enqueued,        // email.received → inbound.fetch enqueued (dedupe in:<id>)
  IgnoredUnknown,  // unknown email id / other app's mail / unhandled type (B9)
  Duplicate,       // svix_id already stored: nothing else happened (still 200 to Svix)
  Error,           // malformed data (e.g. missing email_id); recorded as "error:<detail>"
};

// webhook_events.result spelling: "applied", "enqueued", "ignored_unknown", "duplicate", "error".
std::string_view to_string(WebhookResult r);

struct WebhookOutcome {
  WebhookResult result = WebhookResult::IgnoredUnknown;
  std::string detail;  // short machine detail for "error:<detail>" (no payload contents)
};

// 1. INSERT OR IGNORE webhook_events(svix_id, type, resend_email_id, payload=raw_body,
//    received_at=now). Already present → Duplicate (no other effect).
// 2. kv last_webhook_at = now.
// 3. Route by env.type:
//    * "email.received" → mail::record_inbound_pending(resend_id = data.email_id, Webhook) and
//      jobs::enqueue(inbound.fetch, {resend_id, source:"webhook"}, dedupe in:<id>,
//      priority kPriorityNormal) → Enqueued;
//    * other "email.*" → mail::apply_outbound_event with resend_id = data.email_id, uuid =
//      tags["azmail_outbound"], type = env.type, occurred_at = env.created_at_ms (or now),
//      recipients = data.to, detail = data (bounce/failed/delay info), source_key = svix_id,
//      message_id = data.message_id → Applied, or IgnoredUnknown when no outbound matches;
//    * anything else → IgnoredUnknown.
// 4. UPDATE webhook_events SET processed_at=now, result=<to_string or "error:"+detail>.
// Never throws for unknown or malformed payload content (→ Error / IgnoredUnknown); DB errors
// propagate (the handler then answers 500 so Svix retries).
WebhookOutcome process_webhook(db::Tx& tx, const resend::WebhookEnvelope& env,
                               std::string_view svix_id, std::string_view raw_body,
                               int64_t now_ms);

}  // namespace azm::jobs
