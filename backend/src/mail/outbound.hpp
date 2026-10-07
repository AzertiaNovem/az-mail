// Owner: WP-B
//
// Outbound state machine (DESIGN B1–B5, C3, C5, "Outbound pipeline"). WP-B owns the state;
// WP-C's jobs do the network calls between these short transactions. Status changes caused by
// events follow should_apply()/rank() (types.hpp). Every status change records a
// delivery_events row, recomputes the threads of ALL message copies pointing at the outbound
// (one outbound row → N copies, C3) and emits outbound.status (+ threads.changed when folder
// membership changed) to each copy's owner via tx.emit.
// Functions keyed by outbound id are system-internal (jobs/webhooks): never call them with a
// client-supplied id; the *_owner-scoped functions below are the API entry points.
#pragma once

#include "config.hpp"
#include "core/signed_url.hpp"
#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail {

// outbound row (admin views, jobs).
struct OutboundRow {
  int64_t id = 0;
  std::string uuid;
  int64_t sender_user_id = 0;
  int64_t from_address_id = 0;
  OutboundStatus status = OutboundStatus::Queued;
  std::optional<std::string> status_detail;
  std::optional<std::string> error_name;
  int64_t send_after = 0;
  std::optional<int64_t> scheduled_at;
  std::optional<ScheduledVia> scheduled_via;
  std::optional<std::string> resend_id;
  std::optional<std::string> message_id_header;  // normalized
  std::optional<int64_t> parent_outbound_id;
  int64_t total_bytes = 0;
  std::optional<std::string> last_event;
  std::optional<int64_t> last_event_at;
  std::optional<int64_t> job_id;
  int64_t created_at = 0;
  std::optional<int64_t> accepted_at;
  int64_t updated_at = 0;
};

std::optional<OutboundRow> get_outbound(db::Conn& c, int64_t outbound_id);

// Plan for outbound.send (types.hpp OutboundSendPlan). Throws std::out_of_range when the row
// does not exist (the job then gives up: jobs::Permanent).
OutboundSendPlan load_send_plan(db::Conn& c, int64_t outbound_id);

// Conditional `status IN ('queued','sending') → 'sending'` (retries re-enter while sending).
// False when the outbound is canceled / already accepted / missing: the job stops silently.
bool mark_sending(db::Tx& tx, int64_t outbound_id);

// After POST /emails succeeded: resend_id, accepted_at=now, status 'scheduled' (when
// `scheduled`) else 'accepted' (a webhook may already have moved it higher: then only
// resend_id/accepted_at are set), event local.accepted. Enqueuing outbound.fetch_meta is the
// job's responsibility (same transaction).
void mark_accepted(db::Tx& tx, int64_t outbound_id, std::string_view resend_id, bool scheduled);

// Terminal failure: status 'failed', error_name = `name` (Resend error name), status_detail =
// `detail_zh` (e.g. "发送配额已用完"), event local.failed.
void mark_failed(db::Tx& tx, int64_t outbound_id, std::string_view name, std::string_view detail_zh);

// Transient failure while retrying: keeps status 'sending', sets status_detail (e.g.
// "发送重试中") and error_name so the UI can show it; no event row.
void note_send_retry(db::Tx& tx, int64_t outbound_id, std::string_view name, std::string_view detail_zh);

// Resend rejected scheduling (B5): scheduled_via='local', status back to 'queued'. Returns the
// scheduled_at the job must retry at (Retry{scheduled_at - now, count_attempt=false}).
// Throws std::logic_error when the outbound has no scheduled_at.
int64_t switch_to_local_schedule(db::Tx& tx, int64_t outbound_id);

// Applies one delivery event: finds the outbound (resend_id, then uuid), INSERT OR IGNORE
// delivery_events(outbound_id, source_key) (Duplicate when present), updates last_event /
// last_event_at, applies status_for_event(type) when should_apply, sets status_detail from
// bounce/failure details, captures event.message_id via set_outbound_message_id when missing.
// Lost POST response (B4): when matched by uuid and outbound.resend_id IS NULL, stores
// ev.resend_id; if message_id_header is then still NULL, jobs::enqueue(outbound.fetch_meta,
// {outbound_id}, dedupe out:meta:<id>, run_at now + 10 s) — the send job will see
// mark_sending()==false and stop, so this is the only place that learns the resend id.
EventApplyResult apply_outbound_event(db::Tx& tx, const OutboundEvent& ev);

// Records the Message-ID (normalized) when not yet known: outbound.message_id_header and every
// message copy's message_id_header; then for each copy owner adopt_referencing (threads of
// replies that arrived first are merged, B2). Late loopback cleanup (C3): for each owner
// holding an out copy, an existing direction='in' message of that owner with
// message_id_header = msgid (our own mail that looped back before the id was known, e.g. with
// X-AzMail-Ref stripped) is deleted — its is_read/is_starred/labels OR-ed into the out copy,
// its thread recomputed or deleted — and in_inbox=1 is set on the out copy; threads.changed is
// emitted for the affected threads. No-op when already set to the same value.
void set_outbound_message_id(db::Tx& tx, int64_t outbound_id, std::string_view msgid);

// Outbound id by resend_id, else by uuid (B4). nullopt when neither matches.
std::optional<int64_t> find_outbound(db::Conn& c, std::optional<std::string_view> resend_id,
                                     std::optional<std::string_view> uuid);

// Recent non-terminal outbound rows with a resend_id that outbound.reconcile should poll
// (status in accepted|scheduled|sent|delivery_delayed, updated within `max_age_ms`), oldest
// last_event_at first.
struct ReconcileItem {
  int64_t outbound_id = 0;
  std::string resend_id;
  OutboundStatus status = OutboundStatus::Accepted;
};
std::vector<ReconcileItem> outbound_to_reconcile(db::Conn& c, int64_t now_ms, int64_t max_age_ms,
                                                 int limit);

// ---- cancel / reschedule (API, net pool) -----------------------------------------------------
// Three steps so the Resend call happens outside any transaction:
//   tx1 begin_* → (outside) resend call when plan.remote → tx2 finish_*.

struct CancelPlan {
  int64_t outbound_id = 0;
  bool remote = false;            // true: call resend::Client::cancel(resend_id), then finish
  std::string resend_id;
  std::optional<Draft> draft;     // set when the cancel completed locally (remote == false)
};
// Owner's scheduled message → plan. Local scheduling (or a resend-scheduled send still
// 'queued') is canceled right here (like undo_send) and the draft returned.
// Errors: 404 "not_found"; 409 "invalid_state" (not a scheduled message, or status 'sending'
// while scheduled_at > now: the scheduling POST is in flight — message "正在提交定时发送，请稍后
// 重试"); 409 "already_sent" only once the send itself is under way or done (status 'sending'
// with scheduled_at ≤ now, 'sent' or later) — and from the handler when Resend rejects the
// cancel (Validation).
CancelPlan begin_cancel_schedule(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t message_id);

// After a successful remote cancel: status 'canceled', event local.canceled, message → draft,
// shared copies deleted, recompute, emits. Returns the draft. 404 "not_found" when the outbound
// is not `owner`'s.
Draft finish_cancel_schedule(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t outbound_id);

struct ReschedulePlan {
  int64_t outbound_id = 0;
  bool remote = false;     // true: resend::Client::update_schedule(resend_id, iso8601_utc(at))
  std::string resend_id;
  int64_t scheduled_at = 0;
};
// Validates the new time (422 "invalid_schedule": outside [now + cfg.schedule_min_lead_sec,
// now + cfg.schedule_max_days]) and state (404 "not_found"; 409 "invalid_state" when not
// scheduled or the scheduling POST is in flight; 409 "already_sent" — same split as
// begin_cancel_schedule). Local scheduling is applied immediately (outbound
// scheduled_at + jobs::reschedule; remote=false); otherwise returns the remote plan.
ReschedulePlan begin_reschedule(db::Tx& tx, const Config& cfg, int64_t owner, int64_t message_id,
                                int64_t scheduled_at, int64_t now_ms);
// After a successful remote PATCH: scheduled_at updated, event local.rescheduled, fetch_meta
// job moved to scheduled_at + 60 s, recompute, emits.
void finish_reschedule(db::Tx& tx, int64_t owner, int64_t outbound_id, int64_t scheduled_at);

// POST /api/messages/:id/retry: the owner's message whose outbound is 'failed' gets a NEW
// outbound row (new uuid = new Idempotency-Key, payload copied, status queued, send_after=now),
// every copy re-pointed to it, outbound.send enqueued. Errors: 404 "not_found"; 409
// "invalid_state" when the status is not failed.
SendResult retry_failed_send(db::Tx& tx, int64_t owner, int64_t message_id, int64_t now_ms);

// POST /api/admin/outbox/:id/retry: same as retry_failed_send for any failed outbound (admin;
// not owner-scoped). Returns the NEW outbound id (new row, new uuid); the original row stays
// 'failed' (the API answers with the new OutboxRow). 404 "not_found"; 409 "invalid_state".
int64_t admin_retry_outbound(db::Tx& tx, int64_t outbound_id, int64_t now_ms);

}  // namespace azm::mail
