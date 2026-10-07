// Owner: WP-B
//
// Attachment rows and blob registration (DESIGN §2 attachments/blobs, Addendum A data flows).
// BlobStore I/O (put_file, get_bytes, serve, remove) happens OUTSIDE transactions in the
// callers (api/attachments.cpp, jobs); these functions only touch SQLite.
#pragma once

#include "core/blob_store.hpp"
#include "core/signed_url.hpp"
#include "db/sqlite.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace azm::mail {

// INSERT OR IGNORE INTO blobs(sha256, size, storage, created_at). Call in the transaction that
// first references the blob (after put_file/put_bytes succeeded).
void register_blob(db::Tx& tx, const BlobRef& blob, int64_t now_ms);

// POST /api/attachments, after BlobStore::put_file: registers the blob and inserts an
// unattached attachments row (message_id NULL) for `owner`. `filename` is sanitized (path
// separators/control chars removed, ≤ 255 bytes UTF-8 safe, empty → "attachment");
// `content_type` lowercased without parameters (empty/invalid → application/octet-stream).
// Inline uploads get content_id "<random>@azmail" and is_inline=1. Returns the view with
// signed URLs.
AttachmentView create_upload(db::Tx& tx, const SignedUrls& urls, int64_t owner, const BlobRef& blob,
                             std::string_view filename, std::string_view content_type,
                             bool is_inline, int64_t now_ms);

// Ownership lookup for GET /api/files/:id (after the signed-URL check). nullopt when the
// attachment does not exist or is not `owner`'s.
std::optional<AttachmentRecord> find_attachment(db::Conn& c, int64_t owner, int64_t attachment_id);

// Attachments of one owned message, by id.
std::vector<AttachmentRecord> message_attachments(db::Conn& c, int64_t owner, int64_t message_id);

// Raw .eml of an owned INBOUND message (inbound_emails.raw_sha256). nullopt for outbound
// messages, messages without a raw blob, or foreign ids.
std::optional<RawRef> find_raw(db::Conn& c, int64_t owner, int64_t message_id);

// ---- maintenance (system-wide; gc.housekeeping / gc.blobs) ----------------------------------

// Deletes unattached uploads created before `older_than_ms` (at most `limit`). Returns count.
int purge_orphan_uploads(db::Tx& tx, int64_t older_than_ms, int limit);

// Blobs with no attachments row and no inbound_emails.raw_sha256 reference, created before
// `created_before_ms` (cfg.blob_gc_grace_hours), at most `limit`.
std::vector<BlobRef> unreferenced_blobs(db::Conn& c, int64_t created_before_ms, int limit);

// True when the blobs row exists and nothing references it (no attachments row, no
// inbound_emails.raw_sha256). gc.blobs re-checks with this under blob_gc_guard().
bool is_blob_unreferenced(db::Conn& c, std::string_view sha256);

// Deletes the blobs row iff it is still unreferenced; true when deleted.
// gc.blobs order per candidate (DESIGN Addendum A "remove() → delete row"), all under one
// blob_gc_guard() (core/blob_store.hpp) taken for that blob only:
//   R: is_blob_unreferenced → BlobStore::remove (Services::blobs_for(storage), outside any tx;
//   BlobError → logged, row kept so the next run retries) → W: forget_blob_if_unreferenced.
// A crash between remove and the row delete is harmless: remove of a missing object is a no-op
// and writers re-upload a missing object (put_file checks existence under blob_writer_guard).
bool forget_blob_if_unreferenced(db::Tx& tx, std::string_view sha256);

}  // namespace azm::mail
