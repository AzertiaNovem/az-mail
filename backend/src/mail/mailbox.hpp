// Owner: WP-B
//
// Mailbox reads and user actions (DESIGN §3 "Mail domain contract", C6, D6).
// IDOR rule: every function takes `owner` and filters every query on it; an id that exists but
// belongs to someone else behaves exactly like a missing id (nullopt / 404 "not_found").
// "Normal" message = trashed_at IS NULL AND is_spam = 0 (§2). Signed URLs (attachments, raw,
// inline images) are produced here at read time with exp = urls.expiry(azm::now_ms())
// (SignedUrls carries cfg.signed_url_ttl_sec; mail/types.hpp clock convention).
// Writes emit WS hints through tx.emit only (threads.changed, labels.changed).
#pragma once

#include "core/signed_url.hpp"
#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace azm::mail {

// Thread page for one view (folder, label or search). Ordering: (folder-specific last_at DESC,
// id DESC) using the partial indexes; search groups matching messages by thread with the same
// ordering. Folder semantics follow the §2 aggregates (inbox: inbox_count>0; starred;
// scheduled; sent; drafts; all: msg_count+draft_count>0; spam; trash). Label view: threads
// with a normal message carrying the label. `total` is the view's thread count (null for q).
// Throws ApiError(400, "invalid_field", {field:"cursor"}) for a malformed cursor and
// ApiError(404, "not_found") for a label_id the owner does not have.
ThreadPage list_threads(db::Conn& c, int64_t owner, const ThreadQuery& q);

// Full thread with every message (incl. drafts and trashed), html cid:→signed rewritten,
// attachments with signed URLs. nullopt when missing or not owned.
std::optional<ThreadDetail> get_thread(db::Conn& c, const SignedUrls& urls, int64_t owner,
                                       int64_t thread_id);

// One message (same rendering as get_thread). nullopt when missing or not owned.
std::optional<MessageView> get_message(db::Conn& c, const SignedUrls& urls, int64_t owner,
                                       int64_t message_id);

// GET /api/counts.
Counts counts(db::Conn& c, int64_t owner);

// POST /api/threads/actions. Applies `action` to every message of each owned thread:
//   archive: in_inbox=0 · inbox: in_inbox=1 (and not spam/trash) · read / unread · star /
//   unstar (star affects the latest message, unstar all) · trash: trashed_at=now · restore:
//   trashed_at=NULL · spam: is_spam=1, in_inbox=0 · not_spam: is_spam=0, in_inbox=1 for inbound
//   messages · delete_forever: hard-deletes messages already in trash or spam (others untouched)
//   · add_label / remove_label: message_labels rows on normal messages.
// Then recompute_thread for each touched thread and tx.emit(owner, threads.changed, {ids}).
// Unknown / foreign thread ids are skipped. Returns the affected thread ids (deleted threads
// included). Throws ApiError(400, "invalid_field", {field:"label_id"}) when a label action has
// no label_id, ApiError(404, "not_found") when the label is not the owner's.
// (review R2) trash / delete_forever first settle the sender's own copies of pending sends
// (outbound.hpp cancel_pending_send): a queued scheduled send (trash) or any queued send
// (delete_forever) is canceled in the same transaction — with trash the copy becomes a
// (trashed) draft like undo; an immediate send in its undo window is NOT canceled by trash.
// Refused (whole request rolled back) with ApiError(409, "scheduled_send_pending",
// {thread_id, message_id}) when Resend holds the scheduled send (cancel the schedule first) or a
// scheduled send is being submitted, and with ApiError(409, "send_in_progress") when
// delete_forever hits a send that is being POSTed.
std::vector<int64_t> apply_thread_action(db::Tx& tx, int64_t owner, std::span<const int64_t> ids,
                                         ThreadAction action, std::optional<int64_t> label_id);

// PATCH /api/messages/:id: read/starred flags and labels on one message, recompute its thread,
// emit threads.changed. Throws ApiError(404, "not_found") for a foreign/missing message or
// label.
void patch_message(db::Tx& tx, int64_t owner, int64_t message_id, const MessagePatch& patch);

// GET /api/messages/:id/events: the outbound's delivery_events ascending by occurred_at
// (empty for inbound / never-sent). nullopt when the message is missing or not owned.
std::optional<std::vector<DeliveryEventView>> message_events(db::Conn& c, int64_t owner,
                                                             int64_t message_id);

// Label ids on one owned message (empty when missing).
std::vector<int64_t> message_label_ids(db::Conn& c, int64_t owner, int64_t message_id);

// ---- contacts (GET /api/contacts) ----------------------------------------------------------

// Up to `limit` (1..50) suggestions whose name or email contains `q` (case-insensitive;
// empty q = most relevant): team users (kind team), aliases (kind alias), then the owner's
// contacts by score DESC, last_used_at DESC (kind contact). Deduped by email.
std::vector<ContactView> search_contacts(db::Conn& c, int64_t owner, std::string_view q, int limit);

// contacts upsert: score += delta (sent-to +1.0, received-from +0.2), last_used_at = now, name
// updated when non-empty. Local team addresses are skipped (they come from `addresses`).
void upsert_contact(db::Tx& tx, int64_t owner, const Address& addr, double score_delta, int64_t now_ms);

// ---- retention (system-wide; purge.trash job) ----------------------------------------------

struct PurgeResult {
  int64_t messages_deleted = 0;
  int64_t threads_touched = 0;
  bool more = false;  // the limit was hit; call again
};
// Hard-deletes up to `limit` messages trashed more than trash_days ago or spam older than
// spam_days, for all owners; recomputes their threads and emits threads.changed per owner.
// Blobs are left to gc.blobs. Not owner-scoped (system maintenance).
// (review R5) Spam age counts from MAX(date, created_at, updated_at) — arrival for delivered
// spam, the Spam action (which bumps updated_at) for reported mail — never from an old Date
// header alone. (review R2) A sender copy whose send Resend holds (scheduled) or that is being
// POSTed is skipped until that settles; a queued send of a victim is canceled first.
PurgeResult purge_trash(db::Tx& tx, int64_t now_ms, int trash_days, int spam_days, int limit);

}  // namespace azm::mail
