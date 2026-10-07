// Owner: WP-B
//
// Threading and thread aggregates (DESIGN B2, C6, C7, §2 "recompute_thread semantics").
//  * Reference joins work in BOTH directions through message_refs(owner, ref): a new message
//    joins the thread of any message it references, and any existing message that references
//    the new message's Message-ID is merged into its thread (order-independent, B2).
//  * Subject fallback only when: the subject has a reply prefix (Re/Fw/Fwd/回复/答复/转发/回覆/
//    轉寄 followed by ':', '：' or '[n]'), no reference matched, the candidate thread's last_at
//    is within 7 days, and participants overlap (C7).
//  * Aggregates in `threads` are written ONLY by recompute_thread (rebuilt from source).
// All functions run inside the caller's write transaction and are scoped to `owner`.
#pragma once

#include "db/sqlite.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azm::mail {

inline constexpr int64_t kSubjectFallbackWindowMs = 7LL * 24 * 3600 * 1000;

// Keys of a message being placed into a thread.
struct ThreadingKeys {
  std::optional<std::string> message_id;  // normalized Message-ID of the new message
  std::vector<std::string> refs;          // In-Reply-To + References, normalized, deduped
  std::string subject;                    // raw subject (normalize_subject applied inside)
  std::vector<std::string> participants;  // normalized emails of from/to/cc
  int64_t date = 0;                       // message date (fallback window)
};

// Thread id for a new message of `owner`: the thread of a referenced message (merging when the
// refs hit several threads), else the subject fallback (C7), else a newly created empty thread
// (subject/norm_subject set, aggregates zero). Does NOT insert the message; the caller inserts
// it with the returned thread_id, writes message_refs, then calls adopt_referencing (when it
// has a message_id) and recompute_thread.
int64_t assign_thread(db::Tx& tx, int64_t owner, const ThreadingKeys& keys);

// Reverse direction (B2): merges into `thread_id` every thread of `owner` containing a message
// whose message_refs include `message_id` (via merge_threads, so `thread_id` survives).
// Returns the number of threads absorbed.
int adopt_referencing(db::Tx& tx, int64_t owner, int64_t thread_id, std::string_view message_id);

// Moves every message of thread `absorb` into `keep` (both owned by `owner`), deletes `absorb`,
// recomputes `keep`, and emits threads.changed {keep, absorb}. Returns `keep`. No-op when equal.
// Throws std::invalid_argument when either thread does not belong to `owner`.
int64_t merge_threads(db::Tx& tx, int64_t owner, int64_t keep, int64_t absorb);

// Rebuilds every aggregate of the thread from its messages (§2 table): counts, unread,
// starred, sent/draft/scheduled/spam/trash counts, last_at / spam_last_at / trash_last_at,
// snippet + last_message_id (latest normal non-draft), participants_json (max 6, ordered by
// first appearance, unread flag), attachment_count, subject (of the earliest message).
// Deletes the thread when it has no messages. Returns false when the thread was deleted.
// Does not emit (callers emit threads.changed / mail.new).
bool recompute_thread(db::Tx& tx, int64_t owner, int64_t thread_id);

// Lowercased, whitespace-collapsed subject with all leading reply/forward prefixes and list
// tags like "[team]" removed, e.g. "Re: 回复：[ops] 周报" → "周报".
std::string normalize_subject(std::string_view subject);

// True when the subject starts with a reply/forward prefix (C7).
bool has_reply_prefix(std::string_view subject);

// Thread ids of `owner` that contain a message with one of these Message-IDs (via
// messages.message_id_header) or referencing one of them (message_refs). Used by tests and by
// assign_thread.
std::vector<int64_t> threads_for_refs(db::Conn& c, int64_t owner, std::span<const std::string> refs);

}  // namespace azm::mail
