// Owner: WP-B (frozen contract from WP0; additive changes only)
//
// Mail-domain structs and enums (DESIGN §3 "Mail domain contract"). View types mirror the JSON
// shapes in docs/API.md / frontend/src/api/types.ts field by field (serialization lives in
// mail/serde.hpp); input types carry what the API handlers (WP-D) and jobs (WP-C) pass in.
// Conventions: ids are int64; times are ms-epoch UTC; std::optional = JSON null / absent.
// Message-IDs are stored and passed NORMALIZED (no surrounding <>, trimmed): normalize_message_id.
// Clock: mail functions with a now_ms parameter (or a now_ms field in their options, 0 → real
// time) use it for every timestamp and pass it on (jobs::EnqueueOpts::now_ms); functions
// without one (mark_accepted, mark_failed, apply_outbound_event, apply_thread_action,
// create_draft, update_draft, undo_send, finish_cancel_schedule, finish_reschedule,
// set_outbound_message_id, get_thread/get_message signing, …) use azm::now_ms(). Tests that
// mix them set their ManualClock to real time (ManualClock(azm::now_ms())).
#pragma once

#include "core/address.hpp"
#include "core/blob_store.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"

#include <boost/json/object.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azm::mail {

using azm::Address;

// =============================================================================================
// Enums + wire spelling (inline; trivial)
// =============================================================================================

// Thread-list folders (GET /api/threads?folder=…).
enum class Folder { Inbox, Starred, Scheduled, Sent, Drafts, All, Spam, Trash };

inline constexpr std::array<Folder, 8> kAllFolders = {
    Folder::Inbox, Folder::Starred, Folder::Scheduled, Folder::Sent,
    Folder::Drafts, Folder::All, Folder::Spam, Folder::Trash};

inline constexpr std::string_view to_string(Folder f) {
  switch (f) {
    case Folder::Inbox: return "inbox";
    case Folder::Starred: return "starred";
    case Folder::Scheduled: return "scheduled";
    case Folder::Sent: return "sent";
    case Folder::Drafts: return "drafts";
    case Folder::All: return "all";
    case Folder::Spam: return "spam";
    case Folder::Trash: return "trash";
  }
  return "inbox";
}
// Exact, case-sensitive wire names; nullopt otherwise.
inline constexpr std::optional<Folder> parse_folder(std::string_view s) {
  for (Folder f : kAllFolders)
    if (to_string(f) == s) return f;
  return std::nullopt;
}

enum class Direction { In, Out };  // messages.direction 'in' | 'out'
inline constexpr std::string_view to_string(Direction d) { return d == Direction::In ? "in" : "out"; }
inline constexpr std::optional<Direction> parse_direction(std::string_view s) {
  if (s == "in") return Direction::In;
  if (s == "out") return Direction::Out;
  return std::nullopt;
}

enum class DraftMode { New, Reply, ReplyAll, Forward };  // messages.draft_mode
inline constexpr std::string_view to_string(DraftMode m) {
  switch (m) {
    case DraftMode::New: return "new";
    case DraftMode::Reply: return "reply";
    case DraftMode::ReplyAll: return "reply_all";
    case DraftMode::Forward: return "forward";
  }
  return "new";
}
inline constexpr std::optional<DraftMode> parse_draft_mode(std::string_view s) {
  if (s == "new") return DraftMode::New;
  if (s == "reply") return DraftMode::Reply;
  if (s == "reply_all") return DraftMode::ReplyAll;
  if (s == "forward") return DraftMode::Forward;
  return std::nullopt;
}

// outbound.status (DESIGN B3). Monotone by rank(); `canceled` is local-only and terminal.
enum class OutboundStatus {
  Queued, Sending, Accepted, Scheduled, Sent, DeliveryDelayed, Delivered,
  Complained, Bounced, Failed, Suppressed, Canceled,
};

inline constexpr std::array<OutboundStatus, 12> kAllOutboundStatuses = {
    OutboundStatus::Queued,     OutboundStatus::Sending,  OutboundStatus::Accepted,
    OutboundStatus::Scheduled,  OutboundStatus::Sent,     OutboundStatus::DeliveryDelayed,
    OutboundStatus::Delivered,  OutboundStatus::Complained, OutboundStatus::Bounced,
    OutboundStatus::Failed,     OutboundStatus::Suppressed, OutboundStatus::Canceled};

inline constexpr std::string_view to_string(OutboundStatus s) {
  switch (s) {
    case OutboundStatus::Queued: return "queued";
    case OutboundStatus::Sending: return "sending";
    case OutboundStatus::Accepted: return "accepted";
    case OutboundStatus::Scheduled: return "scheduled";
    case OutboundStatus::Sent: return "sent";
    case OutboundStatus::DeliveryDelayed: return "delivery_delayed";
    case OutboundStatus::Delivered: return "delivered";
    case OutboundStatus::Complained: return "complained";
    case OutboundStatus::Bounced: return "bounced";
    case OutboundStatus::Failed: return "failed";
    case OutboundStatus::Suppressed: return "suppressed";
    case OutboundStatus::Canceled: return "canceled";
  }
  return "queued";
}
inline constexpr std::optional<OutboundStatus> parse_outbound_status(std::string_view s) {
  for (OutboundStatus st : kAllOutboundStatuses)
    if (to_string(st) == s) return st;
  return std::nullopt;
}

// Precedence (B3): queued(0) < sending(1) < accepted = scheduled(2) < sent(3) <
// delivery_delayed(4) < delivered(5) < complained(6) < bounced = failed = suppressed(7);
// canceled(8) is terminal.
inline constexpr int rank(OutboundStatus s) {
  switch (s) {
    case OutboundStatus::Queued: return 0;
    case OutboundStatus::Sending: return 1;
    case OutboundStatus::Accepted:
    case OutboundStatus::Scheduled: return 2;
    case OutboundStatus::Sent: return 3;
    case OutboundStatus::DeliveryDelayed: return 4;
    case OutboundStatus::Delivered: return 5;
    case OutboundStatus::Complained: return 6;
    case OutboundStatus::Bounced:
    case OutboundStatus::Failed:
    case OutboundStatus::Suppressed: return 7;
    case OutboundStatus::Canceled: return 8;
  }
  return 0;
}
// An event-driven transition cur → next is applied iff rank strictly increases and the current
// status is not canceled (equal ranks never replace each other).
inline constexpr bool should_apply(OutboundStatus cur, OutboundStatus next) {
  return cur != OutboundStatus::Canceled && rank(next) > rank(cur);
}
// No further status change is expected: bounced, failed, suppressed, canceled.
inline constexpr bool is_terminal(OutboundStatus s) { return rank(s) >= 7; }

// Status implied by a delivery event type ("email.sent" → Sent, "email.scheduled" →
// Scheduled, "email.delivered", "email.delivery_delayed", "email.complained", "email.bounced",
// "email.failed", "email.suppressed"; "local.failed" → Failed, "local.canceled" → Canceled).
// nullopt for informational events (email.opened, email.clicked, local.queued, …).
inline constexpr std::optional<OutboundStatus> status_for_event(std::string_view type) {
  if (type == "email.sent") return OutboundStatus::Sent;
  if (type == "email.scheduled") return OutboundStatus::Scheduled;
  if (type == "email.delivered") return OutboundStatus::Delivered;
  if (type == "email.delivery_delayed") return OutboundStatus::DeliveryDelayed;
  if (type == "email.complained") return OutboundStatus::Complained;
  if (type == "email.bounced") return OutboundStatus::Bounced;
  if (type == "email.failed") return OutboundStatus::Failed;
  if (type == "email.suppressed") return OutboundStatus::Suppressed;
  if (type == "local.failed") return OutboundStatus::Failed;
  if (type == "local.canceled") return OutboundStatus::Canceled;
  return std::nullopt;
}
// Resend GET /emails/{id}.last_event ("delivered") → event type ("email.delivered"), used by
// outbound.reconcile with source_key "poll:<last_event>".
inline std::string event_type_for_last_event(std::string_view last_event) {
  return "email." + std::string(last_event);
}

enum class ScheduledVia { Resend, Local };  // outbound.scheduled_via (B5)
inline constexpr std::string_view to_string(ScheduledVia v) {
  return v == ScheduledVia::Local ? "local" : "resend";
}
inline constexpr std::optional<ScheduledVia> parse_scheduled_via(std::string_view s) {
  if (s == "resend") return ScheduledVia::Resend;
  if (s == "local") return ScheduledVia::Local;
  return std::nullopt;
}

// POST /api/threads/actions `action`.
enum class ThreadAction {
  Archive, Inbox, Read, Unread, Star, Unstar, Trash, Restore, Spam, NotSpam,
  DeleteForever, AddLabel, RemoveLabel,
};
inline constexpr std::array<ThreadAction, 13> kAllThreadActions = {
    ThreadAction::Archive, ThreadAction::Inbox,   ThreadAction::Read,
    ThreadAction::Unread,  ThreadAction::Star,    ThreadAction::Unstar,
    ThreadAction::Trash,   ThreadAction::Restore, ThreadAction::Spam,
    ThreadAction::NotSpam, ThreadAction::DeleteForever, ThreadAction::AddLabel,
    ThreadAction::RemoveLabel};
inline constexpr std::string_view to_string(ThreadAction a) {
  switch (a) {
    case ThreadAction::Archive: return "archive";
    case ThreadAction::Inbox: return "inbox";
    case ThreadAction::Read: return "read";
    case ThreadAction::Unread: return "unread";
    case ThreadAction::Star: return "star";
    case ThreadAction::Unstar: return "unstar";
    case ThreadAction::Trash: return "trash";
    case ThreadAction::Restore: return "restore";
    case ThreadAction::Spam: return "spam";
    case ThreadAction::NotSpam: return "not_spam";
    case ThreadAction::DeleteForever: return "delete_forever";
    case ThreadAction::AddLabel: return "add_label";
    case ThreadAction::RemoveLabel: return "remove_label";
  }
  return "archive";
}
inline constexpr std::optional<ThreadAction> parse_thread_action(std::string_view s) {
  for (ThreadAction a : kAllThreadActions)
    if (to_string(a) == s) return a;
  return std::nullopt;
}
// add_label / remove_label require label_id.
inline constexpr bool needs_label(ThreadAction a) {
  return a == ThreadAction::AddLabel || a == ThreadAction::RemoveLabel;
}

enum class InboundSource { Webhook, Poll, Admin };  // inbound_emails.source
inline constexpr std::string_view to_string(InboundSource s) {
  switch (s) {
    case InboundSource::Webhook: return "webhook";
    case InboundSource::Poll: return "poll";
    case InboundSource::Admin: return "admin";
  }
  return "webhook";
}
inline constexpr std::optional<InboundSource> parse_inbound_source(std::string_view s) {
  if (s == "webhook") return InboundSource::Webhook;
  if (s == "poll") return InboundSource::Poll;
  if (s == "admin") return InboundSource::Admin;
  return std::nullopt;
}

enum class InboundState { Pending, Delivered, Unroutable, Failed };  // inbound_emails.state
inline constexpr std::string_view to_string(InboundState s) {
  switch (s) {
    case InboundState::Pending: return "pending";
    case InboundState::Delivered: return "delivered";
    case InboundState::Unroutable: return "unroutable";
    case InboundState::Failed: return "failed";
  }
  return "pending";
}
inline constexpr std::optional<InboundState> parse_inbound_state(std::string_view s) {
  if (s == "pending") return InboundState::Pending;
  if (s == "delivered") return InboundState::Delivered;
  if (s == "unroutable") return InboundState::Unroutable;
  if (s == "failed") return InboundState::Failed;
  return std::nullopt;
}

enum class ContactKind { Team, Alias, Contact };  // GET /api/contacts items[].kind
inline constexpr std::string_view to_string(ContactKind k) {
  switch (k) {
    case ContactKind::Team: return "team";
    case ContactKind::Alias: return "alias";
    case ContactKind::Contact: return "contact";
  }
  return "contact";
}

// Message.warnings values (C11).
inline constexpr std::string_view kWarnDmarcFail = "dmarc_fail";
inline constexpr std::string_view kWarnSpoofedInternal = "spoofed_internal";

// Header / tag names stamped on every send (B2, B4).
inline constexpr std::string_view kHeaderAzmailRef = "X-AzMail-Ref";  // value: outbound uuid
inline constexpr std::string_view kTagOutbound = "azmail_outbound";   // value: outbound uuid

// Trim + strip one pair of surrounding <> ("<a@b>" → "a@b"); empty result → "".
inline std::string normalize_message_id(std::string_view raw) {
  auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  std::size_t b = 0, e = raw.size();
  while (b < e && is_ws(raw[b])) ++b;
  while (e > b && is_ws(raw[e - 1])) --e;
  if (e - b >= 2 && raw[b] == '<' && raw[e - 1] == '>') {
    ++b;
    --e;
    while (b < e && is_ws(raw[b])) ++b;
    while (e > b && is_ws(raw[e - 1])) --e;
  }
  return std::string(raw.substr(b, e - b));
}

// (additive, review R1/SEC-8) normalize_message_id plus the checks every stored or sent
// Message-ID / Content-ID must pass: invalid UTF-8 is replaced (utf8_sanitize, the rule
// threads_for_refs also applies to lookups), and the result is "" (= no id) when the value is
// longer than 998 bytes or contains whitespace, a control character (< 0x21, 0x7f) or one of
// < > " \ — such a value cannot be sent back in an In-Reply-To / References / Content-ID header
// safely, and stored ids must never make the JSON they end up in unparseable.
inline constexpr std::size_t kMaxMessageIdBytes = 998;
inline std::string sanitize_message_id(std::string_view raw) {
  std::string id = utf8_sanitize(normalize_message_id(raw));
  if (id.size() > kMaxMessageIdBytes) return {};
  for (const char ch : id) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x21 || u == 0x7f || ch == '<' || ch == '>' || ch == '"' || ch == '\\') return {};
  }
  return id;
}

// =============================================================================================
// Thread list (GET /api/threads → ThreadListResponse)
// =============================================================================================

struct ThreadQuery {
  // Exactly one view selector; when none is set the handler defaults to Folder::Inbox.
  std::optional<Folder> folder;
  std::optional<int64_t> label_id;  // label view (must be the owner's label → else 404)
  std::optional<std::string> q;     // search (mail/search grammar); total = null
  std::optional<std::string> cursor;  // opaque, from a previous ThreadPage::next_cursor
  int limit = 50;                   // clamped to 1..100 (user_settings.page_size by default)
  int tzoff_min = 0;                // minutes east of UTC (after:/before: in search)
  int64_t now_ms = 0;               // newer_than:/older_than: reference; 0 → azm::now_ms()
};

struct Participant {  // ThreadListItem.participants[]
  std::string name;
  std::string email;
  bool is_me = false;   // an address of the owner (own mailbox or alias)
  bool unread = false;  // this participant authored an unread message in the thread
};

struct AttachmentPreview {  // ThreadListItem.attachments_preview[] (max 3, non-inline)
  int64_t id = 0;
  std::string filename;
  std::string content_type;
};

struct ThreadListItem {
  int64_t id = 0;
  std::string subject;
  std::string snippet;
  std::vector<Participant> participants;  // threads.participants_json (max 6, ordered)
  int64_t message_count = 0;              // threads.msg_count
  int64_t draft_count = 0;                // threads.draft_count
  bool unread = false;                    // unread_count > 0 (folder-aware: inbox_unread / spam_unread)
  bool starred = false;                   // starred_count > 0
  bool has_attachments = false;           // attachment_count > 0
  std::vector<int64_t> label_ids;         // union over the thread's normal messages
  int64_t last_at = 0;                    // folder-aware: last_at / spam_last_at / trash_last_at
  bool in_inbox = false;                  // inbox_count > 0
  std::optional<OutboundStatus> latest_status;  // status of the latest outbound message, if any
  std::optional<int64_t> scheduled_at;    // earliest pending scheduled_at in the thread
  std::vector<AttachmentPreview> attachments_preview;
  // Additive (F16): To + Cc — never Bcc — (deduplicated, max 3, is_me marked; `unread` unused)
  // of the newest sent / scheduled (direction 'out', non-draft) message of the view — the Sent /
  // Scheduled rows show "收件人：…" like Gmail instead of the sender ("我"). Empty when the view
  // has none; then the wire field `to_preview` is omitted.
  std::vector<Participant> to_preview;
};

struct ThreadPage {  // ThreadListResponse
  std::vector<ThreadListItem> items;
  std::optional<std::string> next_cursor;  // null on the last page
  std::optional<int64_t> total;            // folder/label total thread count; null for search
};

// =============================================================================================
// Messages / threads (GET /api/threads/:id, GET /api/messages/:id)
// =============================================================================================

struct AttachmentView {  // API Attachment
  int64_t id = 0;
  std::string filename;
  std::string content_type;
  int64_t size = 0;
  bool is_inline = false;                 // JSON "inline"
  std::optional<std::string> content_id;  // without <>; referenced from html as cid:<content_id>
  std::string download_url;               // signed /api/files/:id?d=a…
  std::optional<std::string> view_url;    // signed d=i URL, only for inline-safe types (render.hpp)
};

struct OutboundView {  // Message.outbound
  int64_t id = 0;                          // outbound.id
  OutboundStatus status = OutboundStatus::Queued;
  std::optional<std::string> status_detail;  // Chinese / bounce text for the UI
  std::optional<int64_t> scheduled_at;
  std::optional<ScheduledVia> scheduled_via;
  std::optional<int64_t> undo_until;  // send_after while status=queued and not scheduled
  std::optional<int64_t> sent_at;     // accepted_at (or email.sent time)
};

struct AuthResults {  // Message.auth (inbound only; null for outbound)
  std::optional<std::string> spf, dkim, dmarc;
};

struct MessageView {  // API Message
  int64_t id = 0;
  int64_t thread_id = 0;
  Direction direction = Direction::In;
  bool is_draft = false;
  Address from;
  std::optional<Address> sent_by;  // shared alias copy: the member who actually sent it
  std::vector<Address> to, cc, bcc, reply_to;
  std::optional<std::string> delivered_to;  // inbound: local address it was delivered to (alias)
  std::string subject;
  std::string snippet;
  int64_t date = 0;
  std::optional<std::string> html;  // cid: rewritten to signed URLs (render.hpp)
  std::optional<std::string> text;
  std::vector<AttachmentView> attachments;
  bool is_read = false;
  bool is_starred = false;
  bool in_inbox = false;
  bool is_spam = false;
  bool trashed = false;              // trashed_at IS NOT NULL
  std::vector<int64_t> label_ids;
  std::optional<AuthResults> auth;   // null for outbound / drafts
  std::vector<std::string> warnings;  // kWarnDmarcFail | kWarnSpoofedInternal
  std::optional<OutboundView> outbound;  // out messages that were queued at least once
  std::optional<std::string> message_id_header;  // normalized (no <>)
  std::optional<std::string> raw_url;  // signed /api/files/raw/:id (inbound with a raw blob only)
};

struct ThreadDetail {  // API ThreadDetail: includes drafts and trashed messages (UI filters)
  int64_t id = 0;
  std::string subject;
  std::vector<int64_t> label_ids;  // union over the thread's messages
  std::vector<MessageView> messages;  // ascending by date, then id
};

// =============================================================================================
// Counts (GET /api/counts) — thread counts
// =============================================================================================

struct LabelCount {
  int64_t unread = 0;  // threads with an unread normal message carrying the label
  int64_t total = 0;   // threads with a normal message carrying the label
};

struct Counts {
  int64_t inbox_unread = 0;  // threads with inbox_unread > 0
  int64_t drafts = 0;        // threads with draft_count > 0
  int64_t scheduled = 0;     // threads with scheduled_count > 0
  int64_t spam_unread = 0;   // threads with spam_unread > 0
  std::map<int64_t, LabelCount> labels;  // every label of the owner (zeros included)
};

// =============================================================================================
// Mutations
// =============================================================================================

struct MessagePatch {  // PATCH /api/messages/:id
  std::optional<bool> is_read;
  std::optional<bool> is_starred;
  std::vector<int64_t> add_label_ids;     // each must be the owner's label (else 404)
  std::vector<int64_t> remove_label_ids;
};

// POST /api/drafts, PUT /api/drafts/:id, POST /api/drafts/:id/send {draft}. Absent fields keep
// the stored value (defaults on create); present fields replace it. Nested optionals encode
// JSON null: outer = key present, inner = value or null.
struct DraftInput {
  std::optional<DraftMode> mode;                           // create only; default New
  std::optional<std::optional<int64_t>> parent_message_id;  // create only; owner's message
  std::optional<int64_t> from_address_id;  // one of the owner's identities (403 send_as_forbidden)
  std::optional<std::vector<Address>> to, cc, bcc;
  std::optional<std::string> subject;
  std::optional<std::string> html;  // may contain signed URLs / data-att-id; stored with cid:
  std::optional<std::optional<std::string>> quoted_html;  // kept outside the editor (C10)
  std::optional<std::vector<int64_t>> attachment_ids;  // full list: owner's unattached uploads or
                                                       // attachments already on this draft
  std::optional<bool> include_parent_attachments;      // create only (forward): copy parent's
};

struct Draft {  // API Draft
  int64_t id = 0;  // messages.id of the draft
  int64_t thread_id = 0;
  int64_t version = 0;  // messages.draft_version (optimistic concurrency)
  DraftMode mode = DraftMode::New;  // always present on the wire
  std::optional<int64_t> parent_message_id;
  int64_t from_address_id = 0;
  std::vector<Address> to, cc, bcc;
  std::string subject;
  std::string html;  // cid: rewritten to signed URLs (read-time)
  std::optional<std::string> quoted_html;
  std::vector<AttachmentView> attachments;
  int64_t updated_at = 0;
};

// POST /api/drafts/:id/send body {version, draft?, scheduled_at?}.
struct SendOptions {
  int64_t version = 0;                // must equal the draft's version (409 version_conflict)
  std::optional<DraftInput> draft;    // final field values, saved atomically with the send
  std::optional<int64_t> scheduled_at;  // null/absent = send after the undo window
  int64_t now_ms = 0;                 // 0 → azm::now_ms() (tests inject)
};

struct SendResult {  // API SendResult
  int64_t message_id = 0;   // the (former draft) message, now direction 'out'
  int64_t thread_id = 0;
  int64_t outbound_id = 0;
  OutboundStatus status = OutboundStatus::Queued;
  int64_t undo_ms = 0;      // relative undo window (0 when scheduled or undo disabled)
  std::optional<int64_t> scheduled_at;
};

// =============================================================================================
// Outbound pipeline (WP-B state, WP-C network)
// =============================================================================================

struct PlanAttachment {
  int64_t attachment_id = 0;
  std::string blob_sha256;
  std::string storage;  // blobs.storage "local" | "r2" (Services::blobs_for)
  int64_t size = 0;
  std::string filename;
  std::string content_type;
  std::optional<std::string> content_id;  // inline images
};

// Everything outbound.send needs, rebuilt from outbound.payload_json + attachments + parent at
// load time (so a parent Message-ID captured since the freeze is picked up).
struct OutboundSendPlan {
  int64_t outbound_id = 0;
  std::string uuid;                   // Idempotency-Key, X-AzMail-Ref, tag azmail_outbound
  OutboundStatus status = OutboundStatus::Queued;
  int64_t sender_user_id = 0;
  int64_t from_address_id = 0;
  std::string from;                   // formatted "Name <addr>" (format_address)
  std::vector<std::string> to, cc, bcc, reply_to;  // formatted addresses
  std::string subject;
  std::string html;                   // frozen: body (signature inserted by the client) + quoted_html, cid: kept
  std::string text;                   // html_to_text(html)
  std::vector<std::pair<std::string, std::string>> headers;  // X-AzMail-Ref, In-Reply-To, References
  std::vector<std::pair<std::string, std::string>> tags;     // {azmail_outbound, uuid}
  std::vector<PlanAttachment> attachments;
  std::optional<int64_t> scheduled_at;
  std::optional<ScheduledVia> scheduled_via;
  int64_t total_bytes = 0;            // raw attachment bytes
  // Reply threading (B2): set when the parent is our own outbound whose Message-ID is still
  // unknown; In-Reply-To/References are then missing the parent id in `headers`.
  bool parent_message_id_missing = false;
  std::optional<int64_t> parent_outbound_id;
  std::optional<std::string> parent_resend_id;  // GET /emails/{id} to capture it
  // (additive, review RT-3/R6) In-Reply-To/References were fixed in payload_json before the
  // first POST (mail::freeze_send_headers): every attempt under the same Idempotency-Key sends
  // exactly these headers and no parent lookup happens any more (parent_message_id_missing is
  // then false).
  bool headers_frozen = false;
};

// A delivery event from a webhook, the reconcile poller or a local transition.
struct OutboundEvent {
  std::optional<std::string> resend_id;  // data.email_id (primary correlation)
  std::optional<std::string> uuid;       // tags.azmail_outbound (fallback, B4)
  std::string type;                      // "email.delivered", …, "local.failed"
  int64_t occurred_at = 0;
  std::vector<std::string> recipients;   // data.to
  boost::json::object detail;            // stored as delivery_events.detail_json (bounce text …)
  std::string source_key;                // svix_id | "poll:"+last_event | "local:"+n (UNIQUE per outbound)
  std::optional<std::string> message_id;  // data.message_id → set_outbound_message_id
};

enum class EventOutcome {
  Applied,    // delivery_events row inserted (status changed iff `status_changed`)
  Duplicate,  // (outbound_id, source_key) already recorded
  Unknown,    // no outbound matches resend_id / uuid (B9)
};
struct EventApplyResult {
  EventOutcome outcome = EventOutcome::Unknown;
  std::optional<int64_t> outbound_id;
  bool status_changed = false;
  std::optional<OutboundStatus> status;  // status after applying
};

// =============================================================================================
// Inbound pipeline
// =============================================================================================

struct InboundAttachment {
  std::string resend_attachment_id;
  BlobRef blob;                     // already stored (BlobStore::put_file) before the transaction
  std::string filename;
  std::string content_type;
  std::string disposition;          // "inline" | "attachment"
  std::optional<std::string> content_id;  // without <>
};

// Normalized input to deliver_inbound (built by jobs from resend::ReceivedEmail + the raw
// header block parsed by mail/eml). Never contains Resend's bcc[] (C2).
struct InboundEmail {
  std::string resend_id;            // inbound_emails.resend_id (idempotency key)
  Address from;
  std::vector<Address> to, cc, reply_to;
  std::vector<std::string> received_for;  // envelope recipients (C1): routing truth
  std::string subject;              // decoded UTF-8
  std::optional<std::string> html;  // html_format=cid: inline parts referenced as cid:
  std::string html_format = "cid";
  std::optional<std::string> text;
  std::optional<std::string> message_id;   // normalized
  std::optional<std::string> in_reply_to;  // normalized
  std::vector<std::string> references;     // normalized, in header order
  std::optional<std::string> x_azmail_ref;  // outbound uuid when the mail is our own loopback
  std::optional<std::string> auto_submitted;
  int64_t date = 0;                 // Date header, else received_at
  int64_t received_at = 0;          // Resend created_at
  AuthResults auth;
  std::optional<BlobRef> raw;       // raw .eml blob (absent when Resend had none)
  std::vector<InboundAttachment> attachments;
  InboundSource source = InboundSource::Webhook;
};

struct DeliveryOptions {
  bool unroutable_to_admins = false;  // cfg.unroutable_to_admins (C4)
  int64_t now_ms = 0;                 // 0 → azm::now_ms()
};

struct DeliveredCopy {
  int64_t owner_id = 0;
  int64_t message_id = 0;
  int64_t thread_id = 0;
  std::string delivered_to;   // local address that routed it (user mailbox or alias)
  bool in_inbox = false;
  bool is_spam = false;
  bool loopback_merged = false;  // existing outbound copy got in_inbox=1 instead of a new row (C3)
};

struct DeliveryResult {
  enum class State {
    Delivered,   // at least one copy created or merged
    Unroutable,  // no local recipient (inbound_emails.state='unroutable')
    Duplicate,   // already delivered earlier (idempotent re-run): nothing changed
  } state = State::Unroutable;
  int64_t inbound_id = 0;  // inbound_emails.id
  std::vector<DeliveredCopy> copies;
};

// A local recipient after envelope routing (C1/C3).
struct LocalRecipient {
  int64_t user_id = 0;
  int64_t address_id = 0;     // address that matched (user mailbox or alias)
  std::string delivered_to;   // that address's email
  bool via_alias = false;
};

// =============================================================================================
// Misc read models
// =============================================================================================

struct DeliveryEventView {  // GET /api/messages/:id/events → {events:[…]}
  std::string type;
  int64_t occurred_at = 0;
  boost::json::object detail;  // parsed detail_json ({} when empty)
};

struct ContactView {  // GET /api/contacts items[]
  std::string name;
  std::string email;
  ContactKind kind = ContactKind::Contact;
};

// An attachments row joined with its blob (ownership lookups for /api/files, freezing, GC).
struct AttachmentRecord {
  int64_t id = 0;
  int64_t owner_id = 0;
  std::optional<int64_t> message_id;  // null = uploaded, not yet attached
  std::string blob_sha256;
  std::string storage;                // blobs.storage
  std::string filename;
  std::string content_type;
  int64_t size = 0;
  std::optional<std::string> content_id;
  bool is_inline = false;
  std::optional<std::string> resend_attachment_id;
  int64_t created_at = 0;
};

// Raw .eml of an inbound message (GET /api/messages/:id/raw, /api/files/raw/:id).
struct RawRef {
  int64_t message_id = 0;
  std::string sha256;
  std::string storage;  // blobs.storage
  int64_t size = 0;
  std::string filename;  // suggested download name, e.g. "<subject>.eml" (sanitized)
};

// Search compiled to SQL (mail/search.hpp). `where` is a boolean SQL expression over the alias
// `m` (messages) with positional '?' placeholders bound in order from `binds`; the caller adds
// "m.owner_id = ?" itself. Unless `include_spam_trash`, `where` already excludes spam and trash.
struct SqlFilter {
  std::string where = "1";
  std::vector<db::Value> binds;
  bool include_spam_trash = false;    // in:spam | in:trash | in:anywhere present
  std::optional<Folder> in_folder;    // last in: operator (for list semantics), if any
};

}  // namespace azm::mail
