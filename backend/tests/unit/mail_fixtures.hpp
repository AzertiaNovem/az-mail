// Owner: WP-B — test fixtures for the mail domain (raw-SQL seeding of mail rows).
//
// Read-side tests insert messages directly, the way deliver_inbound / queue_send do: message row
// (+ body, refs, attachments, labels), with the thread chosen by mail::assign_thread, then
// adopt_referencing, recompute_thread and fts_reindex — the sequence DESIGN §3 "deliver_inbound"
// prescribes. Write-side tests (WP-B2) use the real functions via send_fixtures.hpp.
#pragma once

#include "core/address.hpp"
#include "core/crypto.hpp"
#include "db/sqlite.hpp"
#include "mail/attachments.hpp"
#include "mail/fts.hpp"
#include "mail/internal.hpp"
#include "mail/threads.hpp"
#include "mail/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azm::test::mailfx {

inline constexpr int64_t kDay = 24LL * 3600 * 1000;
inline constexpr int64_t kT0 = 1'780'000'000'000;  // 2026-05-28, an arbitrary fixed base time

struct Att {
  std::string filename = "report.pdf";
  std::string content_type = "application/pdf";
  int64_t size = 1234;
  std::optional<std::string> content_id;
  bool is_inline = false;
  std::string content = "attachment-bytes";  // hashed into the blob sha
};

struct Msg {
  int64_t owner = 0;
  std::optional<int64_t> thread_id;  // explicit thread; otherwise assign_thread decides
  std::string direction = "in";
  bool is_draft = false;
  Address from{"Bob External", "bob@ext.example"};
  std::vector<Address> to, cc, bcc, reply_to;
  std::string subject = "hello";
  std::string snippet;  // defaults to the subject
  int64_t date = kT0;
  std::optional<std::string> message_id;
  std::vector<std::string> refs;
  bool is_read = false, is_starred = false, in_inbox = true, is_spam = false;
  std::optional<int64_t> trashed_at;
  std::optional<std::string> html, text;
  std::vector<Att> atts;
  std::optional<int64_t> outbound_id;
  std::optional<int64_t> inbound_id;
  bool is_shared_copy = false;
  std::optional<int64_t> sent_by_user_id;
  std::optional<std::string> delivered_to;
  int64_t size_bytes = 0;
  std::vector<int64_t> labels;
  std::optional<std::string> auth_dmarc;
  std::string warnings_json = "[]";
};

struct Inserted {
  int64_t message_id = 0;
  int64_t thread_id = 0;
  std::vector<int64_t> attachment_ids;
};

inline Inserted insert_message(db::Tx& tx, const Msg& m) {
  std::vector<std::string> participants{normalize_email(m.from.email)};
  for (const auto* list : {&m.to, &m.cc})
    for (const auto& a : *list) participants.push_back(normalize_email(a.email));
  int64_t thread = 0;
  if (m.thread_id) {
    thread = *m.thread_id;
  } else {
    mail::ThreadingKeys keys;
    keys.message_id = m.message_id;
    keys.refs = m.refs;
    keys.subject = m.subject;
    keys.participants = participants;
    keys.date = m.date;
    thread = mail::assign_thread(tx, m.owner, keys);
  }
  const bool has_atts = std::any_of(m.atts.begin(), m.atts.end(), [](const Att& a) { return !a.is_inline; });
  tx.run(
      "INSERT INTO messages(owner_id, thread_id, direction, is_draft, inbound_id, outbound_id, "
      "is_shared_copy, sent_by_user_id, from_name, from_email, to_json, cc_json, bcc_json, "
      "reply_to_json, delivered_to, subject, snippet, date, message_id_header, in_reply_to, "
      "has_attachments, size_bytes, is_read, is_starred, in_inbox, is_spam, trashed_at, auth_dmarc, "
      "warnings_json, created_at, updated_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      m.owner, thread, m.direction, m.is_draft, m.inbound_id, m.outbound_id, m.is_shared_copy,
      m.sent_by_user_id, m.from.name, m.from.email, mail::detail::addresses_to_json(m.to),
      mail::detail::addresses_to_json(m.cc), mail::detail::addresses_to_json(m.bcc),
      mail::detail::addresses_to_json(m.reply_to), m.delivered_to, m.subject,
      m.snippet.empty() ? m.subject : m.snippet, m.date, m.message_id,
      m.refs.empty() ? std::optional<std::string>{} : std::optional<std::string>{m.refs.back()}, has_atts,
      m.size_bytes, m.is_read, m.is_starred, m.in_inbox, m.is_spam, m.trashed_at, m.auth_dmarc,
      m.warnings_json, m.date, m.date);
  Inserted out;
  out.message_id = tx.last_insert_id();
  out.thread_id = thread;
  if (m.html || m.text)
    tx.run("INSERT INTO message_bodies(message_id, html, text) VALUES(?,?,?)", out.message_id, m.html, m.text);
  for (const auto& r : m.refs)
    tx.run("INSERT OR IGNORE INTO message_refs(message_id, owner_id, ref) VALUES(?,?,?)", out.message_id, m.owner,
           mail::normalize_message_id(r));
  for (const auto& a : m.atts) {
    const std::string sha = crypto::sha256_hex(a.content);
    mail::register_blob(tx, BlobRef{sha, static_cast<int64_t>(a.content.size()), "local"}, m.date);
    tx.run(
        "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, "
        "content_id, is_inline, created_at) VALUES(?,?,?,?,?,?,?,?,?)",
        m.owner, out.message_id, sha, a.filename, a.content_type, a.size, a.content_id, a.is_inline, m.date);
    out.attachment_ids.push_back(tx.last_insert_id());
  }
  for (int64_t l : m.labels)
    tx.run("INSERT INTO message_labels(message_id, label_id) VALUES(?,?)", out.message_id, l);
  if (m.message_id && !m.thread_id) mail::adopt_referencing(tx, m.owner, thread, *m.message_id);
  // adopt_referencing may have merged threads; the message's thread is authoritative.
  out.thread_id = *tx.scalar<int64_t>("SELECT thread_id FROM messages WHERE id = ?", out.message_id);
  mail::recompute_thread(tx, m.owner, out.thread_id);
  mail::fts_reindex(tx, out.message_id);
  return out;
}

inline int64_t insert_label(db::Tx& tx, int64_t owner, std::string_view name) {
  tx.run("INSERT INTO labels(owner_id, name, created_at) VALUES(?,?,1)", owner, name);
  return tx.last_insert_id();
}

// An outbound row (status as given). Returns outbound.id.
inline int64_t insert_outbound(db::Tx& tx, int64_t sender, int64_t from_address_id, std::string_view status,
                               std::optional<int64_t> scheduled_at = {}, int64_t send_after = kT0,
                               std::optional<int64_t> accepted_at = {}) {
  tx.run(
      "INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, scheduled_at, "
      "scheduled_via, payload_json, accepted_at, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
      crypto::uuid_v4(), sender, from_address_id, status, send_after, scheduled_at,
      scheduled_at ? std::optional<std::string>{"resend"} : std::optional<std::string>{}, "{}", accepted_at, kT0,
      kT0);
  return tx.last_insert_id();
}

inline int64_t insert_inbound_email(db::Tx& tx, std::string_view resend_id, std::optional<std::string> raw_sha) {
  tx.run(
      "INSERT INTO inbound_emails(resend_id, state, source, raw_sha256, created_at, updated_at) "
      "VALUES(?, 'delivered', 'webhook', ?, 1, 1)",
      resend_id, raw_sha);
  return tx.last_insert_id();
}

// Aggregates of one thread, for assertions.
struct Agg {
  int64_t msg_count = 0, unread_count = 0, inbox_count = 0, inbox_unread = 0, starred_count = 0,
          sent_count = 0, draft_count = 0, scheduled_count = 0, spam_count = 0, spam_unread = 0,
          trash_count = 0, attachment_count = 0, last_at = 0, spam_last_at = 0, trash_last_at = 0;
  std::optional<int64_t> last_message_id;
  std::string snippet, subject, norm_subject, participants_json;
};

inline std::optional<Agg> aggregates(db::Conn& c, int64_t thread_id) {
  auto s = c.prepare(
      "SELECT msg_count, unread_count, inbox_count, inbox_unread, starred_count, sent_count, draft_count, "
      "scheduled_count, spam_count, spam_unread, trash_count, attachment_count, last_at, spam_last_at, "
      "trash_last_at, last_message_id, snippet, subject, norm_subject, participants_json FROM threads WHERE id = ?");
  s.bind_all(thread_id);
  if (!s.step()) return std::nullopt;
  Agg a;
  a.msg_count = s.i64(0);
  a.unread_count = s.i64(1);
  a.inbox_count = s.i64(2);
  a.inbox_unread = s.i64(3);
  a.starred_count = s.i64(4);
  a.sent_count = s.i64(5);
  a.draft_count = s.i64(6);
  a.scheduled_count = s.i64(7);
  a.spam_count = s.i64(8);
  a.spam_unread = s.i64(9);
  a.trash_count = s.i64(10);
  a.attachment_count = s.i64(11);
  a.last_at = s.i64(12);
  a.spam_last_at = s.i64(13);
  a.trash_last_at = s.i64(14);
  a.last_message_id = s.opt_i64(15);
  a.snippet = s.text(16);
  a.subject = s.text(17);
  a.norm_subject = s.text(18);
  a.participants_json = s.text(19);
  return a;
}

inline int64_t thread_of(db::Conn& c, int64_t message_id) {
  return c.scalar<int64_t>("SELECT thread_id FROM messages WHERE id = ?", message_id).value_or(0);
}

inline int64_t count(db::Conn& c, std::string_view sql) {
  return c.scalar<int64_t>(sql).value_or(-1);
}

}  // namespace azm::test::mailfx
