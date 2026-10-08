// Owner: WP-B
//
// Inbound routing and delivery (DESIGN C1–C4, C8, C11, "deliver_inbound"). Pure DB work inside
// the caller's transaction; all downloads and BlobStore writes happened before (WP-C job).
#pragma once

#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail {

// Envelope routing (C1/C3): each email is normalized (lowercase, "+tag" stripped) and matched
// against `addresses` in local domains. A user address yields that user; an alias yields every
// member (alias_members). Users are deduplicated keeping the first match's delivered_to /
// address (order of `emails`). Disabled users are skipped. Non-local or unknown emails are
// ignored.
std::vector<LocalRecipient> resolve_local_recipients(db::Conn& c, std::span<const std::string> emails);

// Emails (as given) whose domain is a local domain but that match no address (C4; used by
// queue_send for 422 "unknown_local_recipient").
std::vector<std::string> unknown_local_recipients(db::Conn& c, std::span<const std::string> emails);

// Idempotent fan-out of one received email (C8):
//  * upsert inbound_emails (resend_id) with metadata (message_id_header, from_email, subject,
//    received_at, raw_sha256, recipients_json = received_for, meta_json); register blobs (raw +
//    attachments);
//  * recipients = resolve_local_recipients(received_for), falling back to to ∪ cc only when
//    received_for is empty; none → active admins when opts.unroutable_to_admins (delivered_to =
//    the first envelope recipient), else state 'unroutable' → DeliveryResult::Unroutable;
//  * per owner: if x_azmail_ref matches an outbound and the owner already has a copy of it →
//    set in_inbox=1 on that copy (loopback merge, C3) and capture the outbound Message-ID via
//    set_outbound_message_id when missing. When x_azmail_ref is absent (stripped by a relay;
//    E2E 08) or matches nothing, but email.message_id equals outbound.message_id_header
//    (outbound_msgid index) of an outbound the owner holds a copy of → the same loopback merge.
//    (A loopback that arrives before the Message-ID is known is inserted normally;
//    set_outbound_message_id merges it later.) Otherwise INSERT OR IGNORE the message (UNIQUE(owner,
//    inbound_id) / messages_in_msgid) — changes()==0 → skip (split delivery / re-run). New
//    copies get body, refs, attachments, label-free, bcc = [owner's address] only when the owner
//    was not in To/Cc (C2), spam + warnings (spam_warnings), assign_thread, adopt_referencing,
//    recompute_thread, fts_reindex, upsert_contact(from, +0.2), and
//    tx.emit(owner, mail.new, ws::mail_new_payload(...));
//  * inbound_emails.state = 'delivered'.
// A re-run after full delivery returns State::Duplicate without changes.
// (review R1/SEC-8) Every Message-ID / In-Reply-To / References / Content-ID is passed through
// sanitize_message_id before it is stored (messages, message_refs, attachments, meta_json), and
// every JSON column is written as valid UTF-8.
// (review R7/SEC-3) A matching X-AzMail-Ref still selects the loopback merge, but it only counts
// as "ours" for spam_warnings when verified — every local envelope recipient is a recipient of
// that send (frozen To/Cc/Bcc) and, once the send's Message-ID is known, the mail carries it —
// and capture source (c) additionally needs DKIM or DMARC "pass" (and no DMARC "fail").
// (review R11) A loopback part whose owner's copy was already merged by an earlier part
// (delivered_to set) changes nothing: no unread flip, no second mail.new.
DeliveryResult deliver_inbound(db::Tx& tx, const InboundEmail& email, const DeliveryOptions& opts = {});

// Pure spoofing rules (C11). Returns warnings: kWarnDmarcFail when auth.dmarc == "fail";
// kWarnSpoofedInternal when `from_is_local` and neither DKIM nor DMARC is "pass" and
// `azmail_ref_matches` (a VERIFIED ref, see deliver_inbound) is false. Any warning ⇒ the copy
// is delivered as spam.
std::vector<std::string> spam_warnings(const AuthResults& auth, bool from_is_local, bool azmail_ref_matches);

// Creates the inbound_emails row in state 'pending' if absent (webhook / poller, before
// enqueuing inbound.fetch). Returns its id either way.
int64_t record_inbound_pending(db::Tx& tx, std::string_view resend_id, InboundSource source, int64_t now_ms);

// state 'failed' with `error` (short, no mail content), e.g. after GET receiving 404 ×5.
void mark_inbound_failed(db::Tx& tx, std::string_view resend_id, std::string_view error, int64_t now_ms);

// Current state of a known inbound email; nullopt when never seen (the poller stops paging at
// the first known id, B8).
std::optional<InboundState> inbound_state(db::Conn& c, std::string_view resend_id);

}  // namespace azm::mail
