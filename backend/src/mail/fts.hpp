// Owner: WP-B
//
// Full-text index maintenance (DESIGN §2 "Keeping FTS in sync"). message_fts rowid == messages.id.
// Field contents: subject; from_text = name + email; to_text = to + cc (+ bcc on the sender's
// own copy only, C2); body = text, or html_to_text(html) when text is null, truncated to
// kFtsBodyLimit bytes (UTF-8 safe); attach_names = filenames joined with ' '.
// Deletes happen through the messages_fts_ad trigger.
#pragma once

#include "db/sqlite.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace azm::mail {

inline constexpr std::size_t kFtsBodyLimit = 256u << 10;  // fixed (Config::fts_body_limit is informational)

// DELETE + INSERT … SELECT for one message, inside the caller's transaction. Call whenever a
// message is inserted, a draft is saved, a send is frozen, an undo happens or attachments
// change. A missing message only deletes the FTS row.
void fts_reindex(db::Tx& tx, int64_t message_id);

// `azmail reindex`: rebuilds the whole index in batches of `batch` messages, each batch in its
// own Pool::write. `progress(done, total)` is called after each batch. Returns messages indexed.
int64_t fts_rebuild_all(db::Pool& pool, int batch = 500,
                        const std::function<void(int64_t, int64_t)>& progress = {});

// One FTS5 string literal: the term wrapped in double quotes with embedded quotes doubled.
std::string fts_quote(std::string_view term);

}  // namespace azm::mail
