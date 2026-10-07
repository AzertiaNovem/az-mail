// Owner: WP-B
//
// Drafts, send queueing and undo (DESIGN C3, C4, C10, C12, "Outbound pipeline").
//  * Optimistic concurrency: messages.draft_version; a mismatch → ApiError(409,
//    "version_conflict", {current: <Draft JSON>}) unless `force`.
//  * Stored-HTML invariant: writes run rewrite_signed_to_cid (render.hpp) on BOTH html and
//    quoted_html so stored HTML only references local attachments as cid:<content_id>; reads
//    (Draft.html and Draft.quoted_html) rewrite cid: back to signed URLs of the draft's
//    attachments. Rewrite targets = the draft's attachments ∪ (reply/reply_all/forward) the
//    parent's inline attachments, mapping the parent attachment id → the same content_id (the
//    frontend builds quoted_html from the cached parent Message, whose images are signed
//    /api/files/<parent_att_id> URLs with data-att-id=<parent_att_id>).
//  * quoted_html is stored separately and appended only at send freeze.
//  * Send-as rule (single definition, also used by repo::identities_for_user): a user may send
//    from their own mailbox address (addresses.kind='user' AND user_id=owner) or from an alias
//    where alias_members(alias_id, user_id=owner).can_send_as=1.
// mail/* reads account tables (addresses, alias_members, domains, user_settings, users,
// labels) with its own SQL and never writes them.
#pragma once

#include "config.hpp"
#include "core/signed_url.hpp"
#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <optional>

namespace azm::mail {

// GET /api/drafts/:id. nullopt when missing, not owned, or not a draft. A NULL
// messages.from_address_id (its alias was deleted: ON DELETE SET NULL) is reported as
// default_from_address(owner).
std::optional<Draft> get_draft(db::Conn& c, const SignedUrls& urls, int64_t owner, int64_t draft_id);

// POST /api/drafts. Defaults: mode New; from = the parent's delivered_to identity when the
// owner may send as it (reply/reply_all), else the owner's own address; thread = the parent's
// thread for reply/reply_all/forward, else a new thread; version 1. For reply/reply_all/forward
// the parent's inline attachments (content_id set) are copied onto the draft (new attachments
// rows on the same blob, same content_id/filename/type, is_inline kept) so quoted images keep
// working as cid: parts; forward with include_parent_attachments copies ALL parent attachments.
// Rewrites html and quoted_html to cid: (targets above), links attachment_ids,
// recompute_thread, fts_reindex, emits threads.changed.
// Errors: 404 "not_found" (parent not owned), 403 "send_as_forbidden" (from_address_id not an
// identity), 400 "invalid_field" {field} (bad attachment ids, too many recipients is NOT
// checked here — only at send).
Draft create_draft(db::Tx& tx, const SignedUrls& urls, int64_t owner, const DraftInput& in);

// PUT /api/drafts/:id: applies present fields (html and quoted_html rewritten to cid: with the
// same targets as create), version+1, same side effects as create.
// Errors: 404 "not_found"; 409 "version_conflict" {current} when version != stored and !force;
// 403 "send_as_forbidden"; 400 "invalid_field".
Draft update_draft(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t id, int64_t version,
                   const DraftInput& in, bool force);

// DELETE /api/drafts/:id: deletes the draft message (attachments rows cascade; blobs → GC),
// recomputes/deletes its thread, emits threads.changed. 404 "not_found" when missing/not a draft.
void delete_draft(db::Tx& tx, int64_t owner, int64_t id);

// POST /api/drafts/:id/send — one transaction (DESIGN "queue_send"):
//  1. optional opts.draft applied like update_draft (version checked first);
//  2. validate: from identity (403 "send_as_forbidden"); ≥ 1 recipient (422 "no_recipients");
//     each of To/Cc/Bcc ≤ cfg.max_recipients_per_field (422 "too_many_recipients" {field});
//     local-domain recipients exist in addresses (422 "unknown_local_recipient" {emails});
//     attachments ≤ cfg.upload_body_limit each and ≤ cfg.max_message_attachment_bytes total
//     (413 "message_too_large"); scheduled_at within [now + cfg.schedule_min_lead_sec,
//     now + cfg.schedule_max_days] (422 "invalid_schedule");
//  3. freeze payload_json (From formatted; html = body + signature (user_settings) +
//     quoted_html (both already cid:, opts.draft's re-run through rewrite_signed_to_cid),
//     then strip_att_ids, then strip_api_file_urls(cfg.public_api_base_url) so no signed
//     /api/files URL ever leaves the server; cid: kept; text = html_to_text of that html;
//     In-Reply-To / References (parent's References + parent id, last 20); X-AzMail-Ref; tag
//     azmail_outbound; attachments by id — except in forward mode, inline attachments
//     (is_inline=1) whose content_id the frozen html does not reference are removed from the
//     message and not sent (copied quote images after the user dropped the quote, inline
//     uploads deleted from the body); forwards keep every attachment);
//  4. INSERT outbound (new uuid, status queued, send_after = now + undo_send_seconds*1000, or
//     now when scheduled; scheduled_via = local when attachments exist or
//     cfg.schedule_mode == Local, else resend);
//  5. the draft becomes direction 'out' with outbound_id; shared alias copies for other members
//     when sending as an alias with share_sent=1 (C3); recompute_thread; fts_reindex; contacts
//     upsert (+1 per recipient);
//  6. jobs::enqueue(outbound.send, {outbound_id}, run_at = send_after (or scheduled_at for
//     local scheduling), dedupe out:send:<id>, priority kPriorityHigh, max_attempts
//     kOutboundSendMaxAttempts); emits threads.changed.
// Errors also: 404 "not_found", 409 "version_conflict" {current: Draft} (built with `urls`,
// like update_draft's conflict, so the client can reload the draft in place).
// api::drafts_send calls this overload with svc.signed_urls.
SendResult queue_send(db::Tx& tx, const Config& cfg, const SignedUrls& urls, int64_t owner,
                      int64_t draft_id, const SendOptions& opts);
// Same without SignedUrls (kept for source compatibility; jobs/tests that never hit a version
// conflict): identical behaviour except a 409 "version_conflict" carries details {} (no
// `current`). Never used by the API.
SendResult queue_send(db::Tx& tx, const Config& cfg, int64_t owner, int64_t draft_id,
                      const SendOptions& opts);

// POST /api/messages/:id/undo-send: UPDATE outbound SET status='canceled' WHERE status='queued'
// (no scheduled_at). 0 rows → 409 "too_late". Otherwise cancels the job, records
// local.canceled, reverts the message to a draft (version+1), deletes shared copies,
// recomputes, emits threads.changed, returns the draft. 404 "not_found" for foreign/missing.
Draft undo_send(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t message_id);

// The identity `owner` may send from (send-as rule above). Throws ApiError(403,
// "send_as_forbidden") otherwise (also for unknown address ids).
struct SenderIdentity {
  int64_t address_id = 0;
  Address address;          // display name from addresses.display_name (or users.display_name)
  bool is_alias = false;
  bool share_sent = false;  // alias copies for other members (C3)
};
SenderIdentity resolve_sender(db::Conn& c, int64_t owner, int64_t address_id);

// The owner's own mailbox address id (addresses.kind='user'). Throws ApiError(404,
// "not_found") when the user has none.
int64_t default_from_address(db::Conn& c, int64_t owner);

}  // namespace azm::mail
