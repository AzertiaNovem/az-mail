// Owner: WP-B (internal helpers shared by drafts.cpp, outbound.cpp and inbound.cpp; not a
// contract header)
//
// The frozen outbound payload (outbound.payload_json), delivery-event bookkeeping, fan-out of
// outbound status changes to every message copy (C3), alias shared copies and the
// "message back to draft" transition used by undo-send and cancel-schedule.
#pragma once

#include "core/address.hpp"
#include "core/signed_url.hpp"
#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail::detail {

// ---- outbound.payload_json ---------------------------------------------------------------------
// Everything the send needs, frozen at queue time (attachments by id, no bytes). The pre-freeze
// draft body (editor html + quoted_html, both cid: form) is kept so undo/cancel can restore the
// draft exactly as it was (the frozen html has the signature and quote merged in).
struct FrozenPayload {
  std::string from;                                // format_address of the identity
  std::vector<std::string> to, cc, bcc, reply_to;  // formatted addresses
  std::string subject;
  std::string html;  // body + signature + quote, data-att-id / API URLs stripped, cid: kept
  std::string text;  // html_to_text(html)
  std::vector<int64_t> attachment_ids;     // the sender copy's attachments rows
  std::optional<int64_t> parent_message_id;  // sender's parent message (messages.id)
  std::optional<std::string> in_reply_to;  // parent Message-ID when known at freeze (normalized)
  std::vector<std::string> references;     // parent's References chain, normalized, WITHOUT the parent id
  std::string draft_html;                  // editor body before the freeze (cid: form)
  std::optional<std::string> draft_quoted_html;
};

std::string payload_to_json(const FrozenPayload& p);
FrozenPayload payload_from_json(std::string_view json);  // tolerant: missing keys → defaults

// The Message-ID of an outbound's parent as known NOW: the frozen value, else what was captured
// since on the sender's parent message, else on the parent outbound row (B2). `missing` is set
// when the parent is our own outbound whose id is still unknown (`parent_resend_id` = its
// Resend id, if any, for GET /emails/{id}).
struct ResolvedParent {
  std::optional<std::string> id;
  bool missing = false;
  std::optional<std::string> parent_resend_id;
};
ResolvedParent resolve_parent_id(db::Conn& c, const FrozenPayload& p, int64_t sender_user_id,
                                 std::optional<int64_t> parent_outbound_id);
// The References chain an outbound sends: its frozen chain + the resolved parent id.
std::vector<std::string> sent_references(db::Conn& c, int64_t outbound_id);

// "<a> <b>" for a References header; "<a>" for In-Reply-To.
std::string format_msgid(std::string_view id);
std::string format_msgid_list(const std::vector<std::string>& ids);
// parent References chain + parent id, deduplicated (first occurrence kept), last 20.
std::vector<std::string> reference_chain(const std::vector<std::string>& base,
                                         const std::optional<std::string>& parent_id);

// ---- outbound status ---------------------------------------------------------------------------

// Folder membership implied by an outbound status (§2 sent_count / scheduled_count rules).
struct StatusClass {
  bool sent = false;
  bool scheduled = false;
  bool operator==(const StatusClass&) const = default;
};
StatusClass status_class(std::string_view status, std::optional<int64_t> scheduled_at);

struct OutboundCopy {
  int64_t owner_id = 0;
  int64_t message_id = 0;
  int64_t thread_id = 0;
  bool shared = false;
};
// Non-draft message copies pointing at the outbound (sender copy first, then by id).
std::vector<OutboundCopy> outbound_copies(db::Conn& c, int64_t outbound_id);

// INSERT OR IGNORE delivery_events; false when (outbound_id, source_key) already exists. An empty
// source_key becomes the next "local:<n>" of that outbound.
bool record_delivery_event(db::Tx& tx, int64_t outbound_id, std::string_view type, int64_t occurred_at,
                           const boost::json::object& detail, std::string source_key = {});

// After a status change: recomputes every copy's thread and emits outbound.status to each copy's
// owner, plus threads.changed (per owner, all their copy threads) when `membership_changed`.
void publish_outbound_change(db::Tx& tx, int64_t outbound_id, bool membership_changed);

// Deletes the alias shared copies of an outbound (C3): their threads are recomputed (or deleted)
// and threads.changed emitted per owner.
void delete_shared_copies(db::Tx& tx, int64_t outbound_id);

// Common tail of undo-send and cancel-schedule, after the caller's conditional transition to
// 'canceled': cancels the pending send job, records local.canceled, emits outbound.status,
// deletes the shared copies and turns the sender copy back into a draft (pre-freeze body
// restored, version + 1, outbound link and Message-ID cleared), recomputes, reindexes and emits
// threads.changed. Returns the draft id. Throws ApiError(404) when the sender copy is gone.
int64_t cancel_to_draft(db::Tx& tx, int64_t owner, int64_t outbound_id, int64_t now_ms);

// Enqueues outbound.send for the outbound (dedupe out:send:<id>, high priority, 10 attempts) at
// `run_at` and stores outbound.job_id. Returns the job id.
int64_t enqueue_send_job(db::Tx& tx, int64_t outbound_id, int64_t run_at, int64_t now_ms);

// ---- small shared helpers ----------------------------------------------------------------------

// True when `html` references cid:<content_id> (ASCII case-insensitive substring test).
bool html_references_cid(std::string_view html, std::string_view content_id);
// True when the domain of `email` is a local domain (domains table).
bool is_local_domain(db::Conn& c, std::string_view email);
// Inserts a threads row with zero aggregates (recompute_thread fills it). Returns its id.
int64_t create_empty_thread(db::Tx& tx, int64_t owner, std::string_view subject, int64_t now_ms);
// Emits threads.changed for `ids` (deduplicated, order kept) to `owner`; no-op when empty.
void emit_threads_changed(db::Tx& tx, int64_t owner, std::vector<int64_t> ids);
// Subject safe for a header: valid UTF-8, CR/LF/TAB → spaces, trimmed.
std::string clean_subject(std::string_view s);

}  // namespace azm::mail::detail
