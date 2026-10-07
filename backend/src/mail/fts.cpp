// Owner: WP-B
// FTS5 index maintenance (fts.hpp, DESIGN §2 "Keeping FTS in sync").
#include "mail/fts.hpp"

#include "core/strings.hpp"
#include "mail/html_text.hpp"
#include "mail/internal.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

void append_addresses(std::string& out, std::string_view json) {
  for (const auto& a : detail::addresses_from_json(json)) {
    if (!out.empty()) out.push_back(' ');
    if (!a.name.empty()) {
      out += a.name;
      out.push_back(' ');
    }
    out += a.email;
  }
}

}  // namespace

std::string fts_quote(std::string_view term) {
  std::string out;
  out.reserve(term.size() + 2);
  out.push_back('"');
  for (char c : term) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

void fts_reindex(db::Tx& tx, int64_t message_id) {
  tx.run("DELETE FROM message_fts WHERE rowid = ?", message_id);

  auto s = tx.prepare(
      "SELECT m.subject, m.from_name, m.from_email, m.to_json, m.cc_json, m.bcc_json, "
      "m.direction, m.is_shared_copy, b.text, b.html "
      "FROM messages m LEFT JOIN message_bodies b ON b.message_id = m.id WHERE m.id = ?");
  s.bind_all(message_id);
  if (!s.step()) return;

  const std::string subject = utf8_sanitize(s.text(0));
  std::string from_text = s.text(1);
  if (!from_text.empty()) from_text.push_back(' ');
  from_text += s.text(2);

  std::string to_text;
  append_addresses(to_text, s.text(3));
  append_addresses(to_text, s.text(4));
  // BCC only on the sender's own copy (C2): inbound copies store at most [self] for display,
  // shared alias copies never carry BCC.
  const bool senders_copy = s.text(6) == "out" && s.i64(7) == 0;
  if (senders_copy) append_addresses(to_text, s.text(5));

  std::string body;
  if (auto text = s.opt_text(8)) body = utf8_sanitize(*text);
  else if (auto html = s.opt_text(9)) body = html_to_text(*html);
  if (body.size() > kFtsBodyLimit) body = utf8_truncate(body, kFtsBodyLimit);

  std::string attach_names;
  {
    auto a = tx.prepare("SELECT filename FROM attachments WHERE message_id = ? ORDER BY id");
    a.bind_all(message_id);
    while (a.step()) {
      if (!attach_names.empty()) attach_names.push_back(' ');
      attach_names += a.text(0);
    }
  }

  tx.run(
      "INSERT INTO message_fts(rowid, subject, from_text, to_text, body, attach_names) "
      "VALUES(?,?,?,?,?,?)",
      message_id, subject, utf8_sanitize(from_text), utf8_sanitize(to_text), body,
      utf8_sanitize(attach_names));
}

int64_t fts_rebuild_all(db::Pool& pool, int batch,
                        const std::function<void(int64_t, int64_t)>& progress) {
  if (batch <= 0) batch = 500;
  const int64_t total =
      pool.read([](db::Conn& c) { return c.scalar<int64_t>("SELECT COUNT(*) FROM messages").value_or(0); });
  int64_t done = 0;
  int64_t last_id = 0;
  for (;;) {
    const auto ids = pool.write([&](db::Tx& tx) {
      std::vector<int64_t> page;
      auto s = tx.prepare("SELECT id FROM messages WHERE id > ? ORDER BY id LIMIT ?");
      s.bind_all(last_id, batch);
      while (s.step()) page.push_back(s.i64(0));
      for (int64_t id : page) fts_reindex(tx, id);
      return page;
    });
    if (ids.empty()) break;
    last_id = ids.back();
    done += static_cast<int64_t>(ids.size());
    if (progress) progress(done, std::max(total, done));
    if (static_cast<int>(ids.size()) < batch) break;
  }
  // Rows whose message no longer exists (only possible after manual surgery; the delete
  // trigger normally keeps them in sync).
  pool.write([](db::Tx& tx) {
    tx.run("DELETE FROM message_fts WHERE rowid NOT IN (SELECT id FROM messages)");
  });
  return done;
}

}  // namespace azm::mail
