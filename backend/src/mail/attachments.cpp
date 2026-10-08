// Owner: WP-B
// Attachment rows, blob registration, ownership lookups and GC queries (attachments.hpp).
#include "mail/attachments.hpp"

#include "core/crypto.hpp"
#include "core/strings.hpp"
#include "mail/internal.hpp"
#include "mail/render.hpp"

#include <string>

namespace azm::mail {
namespace {

constexpr std::string_view kRecordColumns =
    "a.id, a.owner_id, a.message_id, a.blob_sha256, b.storage, a.filename, a.content_type, "
    "a.size, a.content_id, a.is_inline, a.resend_attachment_id, a.created_at";

AttachmentRecord read_record(const db::Stmt& s) {
  AttachmentRecord r;
  r.id = s.i64(0);
  r.owner_id = s.i64(1);
  r.message_id = s.opt_i64(2);
  r.blob_sha256 = s.text(3);
  r.storage = s.text(4);
  r.filename = s.text(5);
  r.content_type = s.text(6);
  r.size = s.i64(7);
  r.content_id = s.opt_text(8);
  r.is_inline = s.boolean(9);
  r.resend_attachment_id = s.opt_text(10);
  r.created_at = s.i64(11);
  return r;
}

// Basename without control characters, ≤ 255 bytes; "" when nothing printable is left.
std::string clean_filename(std::string_view raw, std::size_t max_bytes) {
  std::string name = utf8_sanitize(raw);
  // Browsers / clients sometimes send a full path ("C:\fakepath\a.png"): keep the last part.
  if (const auto sep = name.find_last_of("/\\"); sep != std::string::npos) name = name.substr(sep + 1);
  std::string out;
  out.reserve(name.size());
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7f) continue;
    out.push_back(static_cast<char>(c));
  }
  out = std::string(trim(out));
  out = utf8_truncate(out, max_bytes);
  return std::string(trim(out));
}

// An inbound raw .eml (full body + base64 attachments) is live only while a message delivered
// from it still exists, or while its inbound row is still being processed (pending / failed:
// it may be retried). Once every copy is deleted forever or purged — or the mail was
// unroutable — it is garbage (review R3); inbound_emails rows themselves are kept as history.
constexpr std::string_view kUnreferenced =
    "NOT EXISTS (SELECT 1 FROM attachments a WHERE a.blob_sha256 = b.sha256) AND "
    "NOT EXISTS (SELECT 1 FROM inbound_emails i WHERE i.raw_sha256 = b.sha256 AND "
    "(i.state NOT IN ('delivered','unroutable') OR EXISTS (SELECT 1 FROM messages m WHERE m.inbound_id = i.id)))";

}  // namespace

void register_blob(db::Tx& tx, const BlobRef& blob, int64_t now_ms) {
  const std::string_view storage = blob.storage.empty() ? std::string_view("local") : blob.storage;
  tx.run("INSERT OR IGNORE INTO blobs(sha256, size, storage, created_at) VALUES(?,?,?,?)",
         to_lower_ascii(blob.sha256), blob.size, storage, now_ms);
}

AttachmentView create_upload(db::Tx& tx, const SignedUrls& urls, int64_t owner, const BlobRef& blob,
                             std::string_view filename, std::string_view content_type,
                             bool is_inline, int64_t now_ms) {
  register_blob(tx, blob, now_ms);
  AttachmentRecord r;
  r.owner_id = owner;
  r.blob_sha256 = to_lower_ascii(blob.sha256);
  r.storage = blob.storage.empty() ? "local" : blob.storage;
  r.filename = clean_filename(filename, 255);
  if (r.filename.empty()) r.filename = "attachment";
  r.content_type = detail::normalize_mime(content_type);
  if (r.content_type.empty()) r.content_type = "application/octet-stream";
  r.size = blob.size;
  r.is_inline = is_inline;
  if (is_inline) r.content_id = crypto::hex_encode(crypto::random_bytes(12)) + "@azmail";
  r.created_at = now_ms;
  tx.run(
      "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, "
      "content_id, is_inline, created_at) VALUES(?, NULL, ?, ?, ?, ?, ?, ?, ?)",
      owner, r.blob_sha256, r.filename, r.content_type, r.size, r.content_id, r.is_inline, now_ms);
  r.id = tx.last_insert_id();
  return make_attachment_view(r, urls, owner, urls.expiry(now_ms));
}

std::optional<AttachmentRecord> find_attachment(db::Conn& c, int64_t owner, int64_t attachment_id) {
  auto s = c.prepare("SELECT " + std::string(kRecordColumns) +
                     " FROM attachments a JOIN blobs b ON b.sha256 = a.blob_sha256 "
                     "WHERE a.id = ? AND a.owner_id = ?");
  s.bind_all(attachment_id, owner);
  if (!s.step()) return std::nullopt;
  return read_record(s);
}

std::vector<AttachmentRecord> message_attachments(db::Conn& c, int64_t owner, int64_t message_id) {
  std::vector<AttachmentRecord> out;
  auto s = c.prepare("SELECT " + std::string(kRecordColumns) +
                     " FROM attachments a JOIN blobs b ON b.sha256 = a.blob_sha256 "
                     "WHERE a.message_id = ? AND a.owner_id = ? ORDER BY a.id");
  s.bind_all(message_id, owner);
  while (s.step()) out.push_back(read_record(s));
  return out;
}

std::vector<AttachmentRecord> thread_attachments(db::Conn& c, int64_t owner, int64_t thread_id) {
  std::vector<AttachmentRecord> out;
  auto s = c.prepare("SELECT " + std::string(kRecordColumns) +
                     " FROM attachments a JOIN blobs b ON b.sha256 = a.blob_sha256 "
                     "JOIN messages m ON m.id = a.message_id "
                     "WHERE m.thread_id = ? AND m.owner_id = ? AND a.owner_id = ? ORDER BY a.message_id, a.id");
  s.bind_all(thread_id, owner, owner);
  while (s.step()) out.push_back(read_record(s));
  return out;
}

std::optional<RawRef> find_raw(db::Conn& c, int64_t owner, int64_t message_id) {
  auto s = c.prepare(
      "SELECT m.id, m.subject, i.raw_sha256, b.storage, b.size FROM messages m "
      "JOIN inbound_emails i ON i.id = m.inbound_id JOIN blobs b ON b.sha256 = i.raw_sha256 "
      "WHERE m.id = ? AND m.owner_id = ? AND m.direction = 'in'");
  s.bind_all(message_id, owner);
  if (!s.step()) return std::nullopt;
  RawRef r;
  r.message_id = s.i64(0);
  std::string base = clean_filename(s.text(1), 100);
  if (base.empty()) base = "message";
  r.filename = base + ".eml";
  r.sha256 = s.text(2);
  r.storage = s.text(3);
  r.size = s.i64(4);
  return r;
}

int purge_orphan_uploads(db::Tx& tx, int64_t older_than_ms, int limit) {
  if (limit <= 0) return 0;
  tx.run(
      "DELETE FROM attachments WHERE id IN (SELECT id FROM attachments WHERE message_id IS NULL "
      "AND created_at < ? ORDER BY created_at LIMIT ?)",
      older_than_ms, limit);
  return tx.changes();
}

std::vector<BlobRef> unreferenced_blobs(db::Conn& c, int64_t created_before_ms, int limit) {
  std::vector<BlobRef> out;
  if (limit <= 0) return out;
  auto s = c.prepare("SELECT b.sha256, b.size, b.storage FROM blobs b WHERE b.created_at < ? AND " +
                     std::string(kUnreferenced) + " ORDER BY b.created_at, b.sha256 LIMIT ?");
  s.bind_all(created_before_ms, limit);
  while (s.step()) out.push_back(BlobRef{s.text(0), s.i64(1), s.text(2)});
  return out;
}

bool is_blob_unreferenced(db::Conn& c, std::string_view sha256) {
  return c.scalar<int64_t>("SELECT 1 FROM blobs b WHERE b.sha256 = ? AND " + std::string(kUnreferenced),
                           to_lower_ascii(sha256))
      .has_value();
}

bool forget_blob_if_unreferenced(db::Tx& tx, std::string_view sha256) {
  const std::string sha = to_lower_ascii(sha256);
  if (!tx.scalar<int64_t>("SELECT 1 FROM blobs b WHERE b.sha256 = ? AND " + std::string(kUnreferenced), sha))
    return false;
  // Finished inbound rows keep their metadata but no longer point at the raw (FK to blobs).
  tx.run("UPDATE inbound_emails SET raw_sha256 = NULL WHERE raw_sha256 = ?", sha);
  tx.run("DELETE FROM blobs WHERE sha256 = ?", sha);
  return tx.changes() > 0;
}

}  // namespace azm::mail
