// Owner: WP-B
// Outbound state machine (B1–B5, C3, C5): send plans, status transitions with precedence,
// delivery events, late Message-ID capture, cancel / reschedule / retry.
#include "mail/outbound.hpp"

#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "mail/drafts.hpp"
#include "mail/internal.hpp"
#include "mail/send_internal.hpp"
#include "mail/threads.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

namespace json = boost::json;
using detail::FrozenPayload;

constexpr int64_t kDayMs = 24LL * 3600 * 1000;
constexpr int64_t kFetchMetaDelayMs = 10'000;      // after a send whose Message-ID is unknown
constexpr int64_t kFetchMetaAfterSchedule = 60'000;  // scheduled_at + 60 s
constexpr int64_t kRetryMinLeadMs = 60'000;        // a retried scheduled send keeps its time only if ≥ 60 s ahead
constexpr std::size_t kMaxDetailBytes = 1000;

constexpr std::string_view kOutboundCols =
    "id, uuid, sender_user_id, from_address_id, status, status_detail, error_name, send_after, scheduled_at, "
    "scheduled_via, resend_id, message_id_header, parent_outbound_id, total_bytes, last_event, last_event_at, job_id, "
    "created_at, accepted_at, updated_at";

OutboundRow read_outbound(const db::Stmt& s) {
  OutboundRow r;
  r.id = s.i64(0);
  r.uuid = s.text(1);
  r.sender_user_id = s.i64(2);
  r.from_address_id = s.i64(3);
  r.status = parse_outbound_status(s.text(4)).value_or(OutboundStatus::Queued);
  r.status_detail = s.opt_text(5);
  r.error_name = s.opt_text(6);
  r.send_after = s.i64(7);
  r.scheduled_at = s.opt_i64(8);
  if (auto v = s.opt_text(9)) r.scheduled_via = parse_scheduled_via(*v);
  r.resend_id = s.opt_text(10);
  r.message_id_header = s.opt_text(11);
  r.parent_outbound_id = s.opt_i64(12);
  r.total_bytes = s.i64(13);
  r.last_event = s.opt_text(14);
  r.last_event_at = s.opt_i64(15);
  r.job_id = s.opt_i64(16);
  r.created_at = s.i64(17);
  r.accepted_at = s.opt_i64(18);
  r.updated_at = s.i64(19);
  return r;
}

detail::StatusClass class_of(OutboundStatus st, std::optional<int64_t> scheduled_at) {
  return detail::status_class(to_string(st), scheduled_at);
}

ApiError not_found() { return ApiError::not_found("not_found", "邮件不存在"); }
ApiError invalid_state(std::string msg = "当前状态不允许此操作") {
  return ApiError::conflict("invalid_state", std::move(msg));
}
ApiError already_sent() { return ApiError::conflict("already_sent", "邮件已发出，无法修改"); }

// Sets `status` (and status_detail) and publishes the change to every copy.
void transition(db::Tx& tx, const OutboundRow& row, OutboundStatus next, std::optional<std::string> detail,
                std::optional<std::string> error_name, int64_t now) {
  tx.run("UPDATE outbound SET status = ?, status_detail = ?, error_name = ?, updated_at = ? WHERE id = ?",
         to_string(next), detail, error_name, now, row.id);
  detail::publish_outbound_change(tx, row.id,
                                  class_of(row.status, row.scheduled_at) != class_of(next, row.scheduled_at));
}

// True when some other outbound row already holds this resend id (UNIQUE column).
bool resend_id_taken(db::Conn& c, std::string_view resend_id, int64_t outbound_id) {
  return c.scalar<int64_t>("SELECT 1 FROM outbound WHERE resend_id = ? AND id <> ?", resend_id, outbound_id)
      .has_value();
}

// Human-readable reason from a Resend event `data` (bounce text, failure reason, …).
std::optional<std::string> event_detail_text(const json::object& d) {
  auto str = [](const json::value* v) -> std::optional<std::string> {
    if (v && v->is_string() && !v->as_string().empty()) return std::string(v->as_string());
    return std::nullopt;
  };
  auto nested = [&](std::string_view obj, std::string_view key) -> std::optional<std::string> {
    if (const auto* o = d.if_contains(obj); o && o->is_object()) return str(o->as_object().if_contains(key));
    return std::nullopt;
  };
  std::optional<std::string> text = nested("bounce", "message");
  if (!text) text = nested("failed", "reason");
  if (!text) text = nested("suppressed", "message");
  if (!text) text = nested("suppressed", "reason");
  if (!text) text = nested("delivery_delayed", "message");
  if (!text) text = str(d.if_contains("reason"));
  if (!text) text = str(d.if_contains("message"));
  if (!text) text = str(d.if_contains("error"));
  if (text) *text = utf8_truncate(utf8_sanitize(*text), kMaxDetailBytes);
  return text;
}

// The owner's own (non-shared) sent copy and its outbound, for the API entry points.
struct OwnedSend {
  int64_t message_id = 0;
  int64_t thread_id = 0;
  OutboundRow row;
};

OwnedSend load_owned_send(db::Conn& c, int64_t owner, int64_t message_id) {
  auto s = c.prepare(
      "SELECT id, thread_id, is_draft, is_shared_copy, outbound_id FROM messages WHERE id = ? AND owner_id = ?");
  s.bind_all(message_id, owner);
  if (!s.step()) throw not_found();
  OwnedSend o;
  o.message_id = s.i64(0);
  o.thread_id = s.i64(1);
  const bool draft = s.boolean(2), shared = s.boolean(3);
  const std::optional<int64_t> outbound_id = s.opt_i64(4);
  s.reset();
  if (shared) throw not_found();  // only the sender acts on a send
  if (draft || !outbound_id) throw invalid_state("该邮件不是已发送或定时邮件");
  auto row = get_outbound(c, *outbound_id);
  if (!row || row->sender_user_id != owner) throw not_found();
  o.row = std::move(*row);
  return o;
}

void validate_schedule(const Config& cfg, int64_t at, int64_t now) {
  const int64_t lo = now + static_cast<int64_t>(cfg.schedule_min_lead_sec) * 1000;
  const int64_t hi = now + static_cast<int64_t>(cfg.schedule_max_days) * kDayMs;
  if (at < lo || at > hi)
    throw ApiError::unprocessable("invalid_schedule",
                                  "定时发送时间需在 1 分钟后至 " + std::to_string(cfg.schedule_max_days) + " 天内");
}

// The cancel/reschedule state split shared by both entry points (CONTRACTS §H 36).
enum class ScheduleState { LocalPending, Remote, InFlight, AlreadySent, NotScheduled };

ScheduleState schedule_state(const OutboundRow& r, int64_t now) {
  if (!r.scheduled_at) return ScheduleState::NotScheduled;
  switch (r.status) {
    case OutboundStatus::Queued: return ScheduleState::LocalPending;
    case OutboundStatus::Sending:
      return *r.scheduled_at > now ? ScheduleState::InFlight : ScheduleState::AlreadySent;
    case OutboundStatus::Accepted:
    case OutboundStatus::Scheduled:
      if (r.scheduled_via == ScheduledVia::Resend && r.resend_id && !r.resend_id->empty()) return ScheduleState::Remote;
      return ScheduleState::AlreadySent;
    case OutboundStatus::Canceled: return ScheduleState::NotScheduled;
    default: return ScheduleState::AlreadySent;
  }
}

void throw_for(ScheduleState st) {
  switch (st) {
    case ScheduleState::InFlight: throw invalid_state("正在提交定时发送，请稍后重试");
    case ScheduleState::AlreadySent: throw already_sent();
    case ScheduleState::NotScheduled: throw invalid_state("该邮件不是定时邮件");
    default: break;
  }
}

// New outbound row for a failed send (new uuid = new Idempotency-Key, B1); every copy is
// re-pointed to it and outbound.send enqueued. The failed row stays as history.
int64_t clone_failed_outbound(db::Tx& tx, const OutboundRow& old, int64_t now) {
  const bool keep_schedule = old.scheduled_at && *old.scheduled_at > now + kRetryMinLeadMs;
  const std::optional<int64_t> scheduled_at = keep_schedule ? old.scheduled_at : std::nullopt;
  std::optional<std::string> via;
  if (keep_schedule) via = std::string(to_string(old.scheduled_via.value_or(ScheduledVia::Local)));
  const std::string payload =
      tx.scalar<std::string>("SELECT payload_json FROM outbound WHERE id = ?", old.id).value_or("{}");
  tx.run(
      "INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, scheduled_at, scheduled_via, "
      "parent_outbound_id, payload_json, total_bytes, created_at, updated_at) "
      "VALUES(?, ?, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?)",
      crypto::uuid_v4(), old.sender_user_id, old.from_address_id, now, scheduled_at, via, old.parent_outbound_id,
      payload, old.total_bytes, now, now);
  const int64_t id = tx.last_insert_id();
  tx.run("UPDATE messages SET outbound_id = ?, updated_at = ? WHERE outbound_id = ?", id, now, old.id);
  json::object ev;
  ev["retry_of"] = old.id;
  if (scheduled_at) ev["scheduled_at"] = *scheduled_at;
  detail::record_delivery_event(tx, id, "local.queued", now, ev);
  const int64_t run_at = keep_schedule && via == "local" ? *scheduled_at : now;
  detail::enqueue_send_job(tx, id, run_at, now);
  detail::publish_outbound_change(tx, id, true);
  return id;
}

}  // namespace

// =============================================================================================
// Reads
// =============================================================================================

std::optional<OutboundRow> get_outbound(db::Conn& c, int64_t outbound_id) {
  auto s = c.prepare("SELECT " + std::string(kOutboundCols) + " FROM outbound WHERE id = ?");
  s.bind_all(outbound_id);
  if (!s.step()) return std::nullopt;
  return read_outbound(s);
}

OutboundSendPlan load_send_plan(db::Conn& c, int64_t outbound_id) {
  const auto row = get_outbound(c, outbound_id);
  if (!row) throw std::out_of_range("outbound " + std::to_string(outbound_id) + " not found");
  const FrozenPayload p = detail::payload_from_json(
      c.scalar<std::string>("SELECT payload_json FROM outbound WHERE id = ?", outbound_id).value_or("{}"));

  OutboundSendPlan plan;
  plan.outbound_id = row->id;
  plan.uuid = row->uuid;
  plan.status = row->status;
  plan.sender_user_id = row->sender_user_id;
  plan.from_address_id = row->from_address_id;
  plan.from = p.from;
  plan.to = p.to;
  plan.cc = p.cc;
  plan.bcc = p.bcc;
  plan.reply_to = p.reply_to;
  plan.subject = p.subject;
  plan.html = p.html;
  plan.text = p.text;
  plan.scheduled_at = row->scheduled_at;
  plan.scheduled_via = row->scheduled_via;
  plan.total_bytes = row->total_bytes;
  plan.parent_outbound_id = row->parent_outbound_id;

  // Parent Message-ID: frozen value, else whatever was captured since (B2).
  const detail::ResolvedParent parent = detail::resolve_parent_id(c, p, row->sender_user_id, row->parent_outbound_id);
  const std::optional<std::string>& parent_id = parent.id;
  plan.parent_message_id_missing = parent.missing;
  plan.parent_resend_id = parent.parent_resend_id;

  plan.headers.emplace_back(std::string(kHeaderAzmailRef), row->uuid);
  if (parent_id) plan.headers.emplace_back("In-Reply-To", detail::format_msgid(*parent_id));
  if (const auto chain = detail::reference_chain(p.references, parent_id); !chain.empty())
    plan.headers.emplace_back("References", detail::format_msgid_list(chain));
  plan.tags.emplace_back(std::string(kTagOutbound), row->uuid);

  if (!p.attachment_ids.empty()) {
    auto s = c.prepare(
        "SELECT a.id, a.blob_sha256, b.storage, a.size, a.filename, a.content_type, a.content_id "
        "FROM attachments a JOIN blobs b ON b.sha256 = a.blob_sha256 "
        "WHERE a.id IN (SELECT value FROM json_each(?)) AND a.owner_id = ? ORDER BY a.id");
    s.bind_all(detail::json_ids(p.attachment_ids), row->sender_user_id);
    while (s.step()) {
      PlanAttachment a;
      a.attachment_id = s.i64(0);
      a.blob_sha256 = s.text(1);
      a.storage = s.text(2);
      a.size = s.i64(3);
      a.filename = s.text(4);
      a.content_type = s.text(5);
      a.content_id = s.opt_text(6);
      plan.attachments.push_back(std::move(a));
    }
  }
  return plan;
}

std::optional<int64_t> find_outbound(db::Conn& c, std::optional<std::string_view> resend_id,
                                     std::optional<std::string_view> uuid) {
  if (resend_id && !resend_id->empty())
    if (auto id = c.scalar<int64_t>("SELECT id FROM outbound WHERE resend_id = ?", *resend_id)) return id;
  if (uuid && !uuid->empty())
    if (auto id = c.scalar<int64_t>("SELECT id FROM outbound WHERE uuid = ?", *uuid)) return id;
  return std::nullopt;
}

std::vector<ReconcileItem> outbound_to_reconcile(db::Conn& c, int64_t now_ms, int64_t max_age_ms, int limit) {
  std::vector<ReconcileItem> out;
  if (limit <= 0) return out;
  auto s = c.prepare(
      "SELECT id, resend_id, status FROM outbound WHERE status IN ('accepted','scheduled','sent','delivery_delayed') "
      "AND updated_at >= ? AND resend_id IS NOT NULL ORDER BY COALESCE(last_event_at, 0), id LIMIT ?");
  s.bind_all(now_ms - max_age_ms, limit);
  while (s.step())
    out.push_back({s.i64(0), s.text(1), parse_outbound_status(s.text(2)).value_or(OutboundStatus::Accepted)});
  return out;
}

// =============================================================================================
// Job transitions
// =============================================================================================

bool mark_sending(db::Tx& tx, int64_t outbound_id) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row || (row->status != OutboundStatus::Queued && row->status != OutboundStatus::Sending)) return false;
  const int64_t now = azm::now_ms();
  tx.run("UPDATE outbound SET status = 'sending', updated_at = ? WHERE id = ? AND status IN ('queued','sending')",
         now, outbound_id);
  if (tx.changes() == 0) return false;
  if (row->status == OutboundStatus::Queued) {
    detail::record_delivery_event(tx, outbound_id, "local.sending", now, {});
    detail::publish_outbound_change(tx, outbound_id, false);  // queued and sending are both "pending"
  }
  return true;
}

void mark_accepted(db::Tx& tx, int64_t outbound_id, std::string_view resend_id, bool scheduled) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row) return;
  const int64_t now = azm::now_ms();
  if (!resend_id.empty() && !resend_id_taken(tx.conn(), resend_id, outbound_id))
    tx.run("UPDATE outbound SET resend_id = ? WHERE id = ?", resend_id, outbound_id);
  tx.run("UPDATE outbound SET accepted_at = COALESCE(accepted_at, ?), updated_at = ? WHERE id = ?", now, now,
         outbound_id);
  // 'scheduled' without a scheduled_at would leave the message in no folder at all (§2).
  const bool sched = scheduled && row->scheduled_at.has_value();
  json::object ev;
  ev["resend_id"] = resend_id;
  ev["scheduled"] = sched;
  detail::record_delivery_event(tx, outbound_id, "local.accepted", now, ev);
  const OutboundStatus next = sched ? OutboundStatus::Scheduled : OutboundStatus::Accepted;
  // A webhook may already have moved it further (email.sent before the POST returned).
  if (should_apply(row->status, next)) transition(tx, *row, next, std::nullopt, std::nullopt, now);
}

void mark_failed(db::Tx& tx, int64_t outbound_id, std::string_view name, std::string_view detail_zh) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row || row->status == OutboundStatus::Canceled) return;  // canceled is terminal
  // Once Resend reported the mail as sent (or later), a failing retry of the POST cannot undo
  // that: keep Resend's status.
  if (rank(row->status) >= rank(OutboundStatus::Sent) && row->status != OutboundStatus::Failed) return;
  const int64_t now = azm::now_ms();
  json::object ev;
  ev["name"] = name;
  ev["message"] = detail_zh;
  detail::record_delivery_event(tx, outbound_id, "local.failed", now, ev);
  transition(tx, *row, OutboundStatus::Failed, std::string(detail_zh), std::string(name), now);
}

void note_send_retry(db::Tx& tx, int64_t outbound_id, std::string_view name, std::string_view detail_zh) {
  tx.run(
      "UPDATE outbound SET status_detail = ?, error_name = ?, updated_at = ? WHERE id = ? AND "
      "status IN ('queued','sending')",
      detail_zh, name, azm::now_ms(), outbound_id);
  if (tx.changes() > 0) detail::publish_outbound_change(tx, outbound_id, false);
}

int64_t switch_to_local_schedule(db::Tx& tx, int64_t outbound_id) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row || !row->scheduled_at) throw std::logic_error("switch_to_local_schedule: outbound is not scheduled");
  const int64_t now = azm::now_ms();
  tx.run(
      "UPDATE outbound SET scheduled_via = 'local', status = CASE WHEN status IN ('queued','sending') THEN 'queued' "
      "ELSE status END, updated_at = ? WHERE id = ?",
      now, outbound_id);
  json::object ev;
  ev["scheduled_at"] = *row->scheduled_at;
  detail::record_delivery_event(tx, outbound_id, "local.schedule_local", now, ev);
  detail::publish_outbound_change(tx, outbound_id, false);
  return *row->scheduled_at;
}

// =============================================================================================
// Events (B3, B4)
// =============================================================================================

EventApplyResult apply_outbound_event(db::Tx& tx, const OutboundEvent& ev) {
  EventApplyResult res;
  const int64_t now = azm::now_ms();
  std::optional<int64_t> id;
  bool by_uuid = false;
  if (ev.resend_id && !ev.resend_id->empty())
    id = tx.scalar<int64_t>("SELECT id FROM outbound WHERE resend_id = ?", *ev.resend_id);
  if (!id && ev.uuid && !ev.uuid->empty()) {
    id = tx.scalar<int64_t>("SELECT id FROM outbound WHERE uuid = ?", *ev.uuid);
    by_uuid = id.has_value();
  }
  if (!id) return res;  // other apps' mail on a shared Resend account (B9)
  const OutboundRow row = *get_outbound(tx.conn(), *id);
  res.outbound_id = *id;
  res.status = row.status;

  const int64_t occurred = ev.occurred_at > 0 ? ev.occurred_at : now;
  json::object detail = ev.detail;
  if (!ev.recipients.empty() && !detail.contains("to")) {
    json::array to;
    for (const auto& r : ev.recipients) to.emplace_back(json::string(r));
    detail["to"] = std::move(to);
  }
  if (!detail::record_delivery_event(tx, *id, ev.type, occurred, detail, ev.source_key)) {
    res.outcome = EventOutcome::Duplicate;
    return res;
  }
  res.outcome = EventOutcome::Applied;

  // last_event follows Resend's spelling ("delivered"); out-of-order events don't regress it.
  const std::string short_type =
      istarts_with(ev.type, "email.") ? std::string(ev.type.substr(6)) : std::string(ev.type);
  tx.run(
      "UPDATE outbound SET last_event = ?, last_event_at = ? WHERE id = ? AND "
      "(last_event_at IS NULL OR last_event_at <= ?)",
      short_type, occurred, *id, occurred);

  // Lost POST response (B4): the uuid tag is the only link to the Resend id.
  const bool learned_resend_id = by_uuid && !row.resend_id && ev.resend_id && !ev.resend_id->empty() &&
                                 !resend_id_taken(tx.conn(), *ev.resend_id, *id);
  if (learned_resend_id) tx.run("UPDATE outbound SET resend_id = ? WHERE id = ?", *ev.resend_id, *id);

  if (const auto next = status_for_event(ev.type); next && should_apply(row.status, *next)) {
    std::optional<std::string> text;
    if (*next != OutboundStatus::Sent && *next != OutboundStatus::Delivered && *next != OutboundStatus::Scheduled)
      text = event_detail_text(ev.detail);
    transition(tx, row, *next, text, std::nullopt, now);
    res.status_changed = true;
    res.status = *next;
  }

  if (ev.message_id && !row.message_id_header) set_outbound_message_id(tx, *id, *ev.message_id);

  if (learned_resend_id &&
      !tx.scalar<std::string>("SELECT message_id_header FROM outbound WHERE id = ?", *id).has_value()) {
    jobs::EnqueueOpts opts;
    opts.run_at_ms = now + kFetchMetaDelayMs;
    opts.dedupe_key = jobs::dedupe_fetch_meta(*id);
    opts.now_ms = now;
    json::object payload;
    payload[std::string(jobs::payload::kOutboundId)] = *id;
    jobs::enqueue(tx, jobs::kinds::kOutboundFetchMeta, std::move(payload), opts);
  }
  return res;
}

// =============================================================================================
// Message-ID capture (B2, C3)
// =============================================================================================

void set_outbound_message_id(db::Tx& tx, int64_t outbound_id, std::string_view msgid) {
  const std::string id = normalize_message_id(msgid);
  if (id.empty()) return;
  {
    auto s = tx.prepare("SELECT message_id_header FROM outbound WHERE id = ?");
    s.bind_all(outbound_id);
    if (!s.step() || !s.is_null(0)) return;  // missing, or already known (never overwritten)
  }
  const int64_t now = azm::now_ms();
  tx.run("UPDATE outbound SET message_id_header = ?, updated_at = ? WHERE id = ?", id, now, outbound_id);
  tx.run("UPDATE messages SET message_id_header = ? WHERE outbound_id = ? AND is_draft = 0", id, outbound_id);

  std::map<int64_t, std::vector<int64_t>> touched;
  for (const auto& copy : detail::outbound_copies(tx.conn(), outbound_id)) {
    int64_t thread = copy.thread_id;
    // Late loopback cleanup (CONTRACTS §H 27): our own mail that looped back before its id was
    // known was delivered as an 'in' message; fold it into the out copy.
    struct InCopy {
      int64_t id, thread;
      bool read, starred, inbox, normal;
      std::optional<std::string> delivered_to;
    };
    std::vector<InCopy> loops;
    {
      auto s = tx.prepare(
          "SELECT id, thread_id, is_read, is_starred, in_inbox, (trashed_at IS NULL AND is_spam = 0), delivered_to "
          "FROM messages WHERE owner_id = ? AND direction = 'in' AND message_id_header = ?");
      s.bind_all(copy.owner_id, id);
      while (s.step())
        loops.push_back({s.i64(0), s.i64(1), s.boolean(2), s.boolean(3), s.boolean(4), s.boolean(5), s.opt_text(6)});
    }
    for (const auto& in : loops) {
      tx.run("INSERT OR IGNORE INTO message_labels(message_id, label_id) SELECT ?, label_id FROM message_labels "
             "WHERE message_id = ?",
             copy.message_id, in.id);
      // The sender's own copy stays read; a member's shared copy takes the received copy's state.
      tx.run(
          "UPDATE messages SET is_read = CASE WHEN is_shared_copy = 1 THEN ? ELSE is_read END, "
          "is_starred = MAX(is_starred, ?), in_inbox = MAX(in_inbox, ?), "
          "delivered_to = COALESCE(delivered_to, ?), updated_at = ? WHERE id = ?",
          in.read, in.starred, in.inbox && in.normal, in.delivered_to, now, copy.message_id);
      tx.run("UPDATE messages SET parent_message_id = ? WHERE parent_message_id = ? AND owner_id = ?",
             copy.message_id, in.id, copy.owner_id);
      tx.run("DELETE FROM messages WHERE id = ? AND owner_id = ?", in.id, copy.owner_id);
      touched[copy.owner_id].push_back(in.thread);
      // Whatever was threaded with the loopback copy belongs with the out copy.
      if (in.thread != thread && recompute_thread(tx, copy.owner_id, in.thread))
        merge_threads(tx, copy.owner_id, thread, in.thread);
    }
    // Replies that arrived before the id was known (B2).
    adopt_referencing(tx, copy.owner_id, thread, id);
    recompute_thread(tx, copy.owner_id, thread);
    touched[copy.owner_id].push_back(thread);
  }
  for (auto& [owner, ids] : touched) detail::emit_threads_changed(tx, owner, std::move(ids));
}

// =============================================================================================
// Cancel / reschedule (API, net pool)
// =============================================================================================

CancelPlan begin_cancel_schedule(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t message_id) {
  const int64_t now = azm::now_ms();
  const OwnedSend s = load_owned_send(tx.conn(), owner, message_id);
  CancelPlan plan;
  plan.outbound_id = s.row.id;
  const ScheduleState st = schedule_state(s.row, now);
  if (st == ScheduleState::LocalPending) {
    tx.run("UPDATE outbound SET status = 'canceled', status_detail = NULL, updated_at = ? WHERE id = ? AND "
           "status = 'queued'",
           now, s.row.id);
    if (tx.changes() == 0) throw invalid_state();
    const int64_t draft_id = detail::cancel_to_draft(tx, owner, s.row.id, now);
    plan.draft = get_draft(tx.conn(), urls, owner, draft_id);
    return plan;
  }
  if (st == ScheduleState::Remote) {
    plan.remote = true;
    plan.resend_id = *s.row.resend_id;
    return plan;
  }
  throw_for(st);
  throw invalid_state();
}

Draft finish_cancel_schedule(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t outbound_id) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row || row->sender_user_id != owner) throw not_found();
  if (row->status == OutboundStatus::Canceled) throw invalid_state("该定时邮件已取消");
  const int64_t now = azm::now_ms();
  tx.run("UPDATE outbound SET status = 'canceled', status_detail = NULL, updated_at = ? WHERE id = ?", now,
         outbound_id);
  const int64_t draft_id = detail::cancel_to_draft(tx, owner, outbound_id, now);
  return *get_draft(tx.conn(), urls, owner, draft_id);
}

ReschedulePlan begin_reschedule(db::Tx& tx, const Config& cfg, int64_t owner, int64_t message_id,
                                int64_t scheduled_at, int64_t now_ms) {
  const int64_t now = now_ms > 0 ? now_ms : azm::now_ms();
  validate_schedule(cfg, scheduled_at, now);
  const OwnedSend s = load_owned_send(tx.conn(), owner, message_id);
  ReschedulePlan plan;
  plan.outbound_id = s.row.id;
  plan.scheduled_at = scheduled_at;
  const ScheduleState st = schedule_state(s.row, now);
  if (st == ScheduleState::LocalPending) {
    // Not handed to Resend yet: the send job reads scheduled_at when it runs (resend path) or is
    // itself moved (local path).
    tx.run("UPDATE outbound SET scheduled_at = ?, updated_at = ? WHERE id = ? AND status = 'queued'", scheduled_at,
           now, s.row.id);
    if (tx.changes() == 0) throw invalid_state();
    if (s.row.scheduled_via == ScheduledVia::Local && s.row.job_id) jobs::reschedule(tx, *s.row.job_id, scheduled_at);
    json::object ev;
    ev["scheduled_at"] = scheduled_at;
    detail::record_delivery_event(tx, s.row.id, "local.rescheduled", now, ev);
    detail::publish_outbound_change(tx, s.row.id, true);
    return plan;
  }
  if (st == ScheduleState::Remote) {
    plan.remote = true;
    plan.resend_id = *s.row.resend_id;
    return plan;
  }
  throw_for(st);
  throw invalid_state();
}

void finish_reschedule(db::Tx& tx, int64_t owner, int64_t outbound_id, int64_t scheduled_at) {
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row || row->sender_user_id != owner) throw not_found();
  const int64_t now = azm::now_ms();
  tx.run("UPDATE outbound SET scheduled_at = ?, updated_at = ? WHERE id = ?", scheduled_at, now, outbound_id);
  json::object ev;
  ev["scheduled_at"] = scheduled_at;
  detail::record_delivery_event(tx, outbound_id, "local.rescheduled", now, ev);
  // The Message-ID only exists once Resend actually sends: fetch it a minute after the new time.
  // enqueue returns the existing pending/running job under the dedupe key; a pending one is
  // moved (a running one finishes and the reconcile poller covers the rest).
  const int64_t meta_at = scheduled_at + kFetchMetaAfterSchedule;
  jobs::EnqueueOpts opts;
  opts.run_at_ms = meta_at;
  opts.dedupe_key = jobs::dedupe_fetch_meta(outbound_id);
  opts.now_ms = now;
  json::object payload;
  payload[std::string(jobs::payload::kOutboundId)] = outbound_id;
  const int64_t job = jobs::enqueue(tx, jobs::kinds::kOutboundFetchMeta, std::move(payload), opts);
  jobs::reschedule(tx, job, meta_at);
  detail::publish_outbound_change(tx, outbound_id, true);
}

// =============================================================================================
// Retry
// =============================================================================================

SendResult retry_failed_send(db::Tx& tx, int64_t owner, int64_t message_id, int64_t now_ms) {
  const int64_t now = now_ms > 0 ? now_ms : azm::now_ms();
  const OwnedSend s = load_owned_send(tx.conn(), owner, message_id);
  if (s.row.status != OutboundStatus::Failed) throw invalid_state("只有发送失败的邮件可以重试");
  const int64_t id = clone_failed_outbound(tx, s.row, now);
  SendResult r;
  r.message_id = s.message_id;
  r.thread_id = tx.scalar<int64_t>("SELECT thread_id FROM messages WHERE id = ?", s.message_id).value_or(s.thread_id);
  r.outbound_id = id;
  r.status = OutboundStatus::Queued;
  r.undo_ms = 0;
  r.scheduled_at = tx.scalar<int64_t>("SELECT scheduled_at FROM outbound WHERE id = ?", id);
  return r;
}

int64_t admin_retry_outbound(db::Tx& tx, int64_t outbound_id, int64_t now_ms) {
  const int64_t now = now_ms > 0 ? now_ms : azm::now_ms();
  const auto row = get_outbound(tx.conn(), outbound_id);
  if (!row) throw ApiError::not_found("not_found", "发送记录不存在");
  if (row->status != OutboundStatus::Failed) throw invalid_state("只有发送失败的邮件可以重试");
  return clone_failed_outbound(tx, *row, now);
}

}  // namespace azm::mail
