// Owner: WP-B
// Threading (B2, C7) and thread aggregates (C6, §2 recompute_thread table).
#include "mail/threads.hpp"

#include "core/address.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/internal.hpp"
#include "mail/types.hpp"
#include "ws/events.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

constexpr std::size_t kMaxParticipants = 6;
constexpr std::size_t kMaxRefsLookup = 200;  // bound the lookup for pathological References

// Skips ASCII whitespace, U+00A0 and U+3000 starting at i.
std::size_t skip_spaces(std::string_view s, std::size_t i) {
  while (i < s.size()) {
    const std::size_t n = detail::unicode_space_len(s, i);
    if (n == 0) break;
    i += n;
  }
  return i;
}

// Reply / forward prefix tokens (C7). ASCII ones match case-insensitively.
constexpr std::array<std::string_view, 9> kPrefixTokens = {
    "fwd", "re", "fw", "回复", "答复", "转发", "回覆", "轉寄", "回復"};

// Length of a reply/forward prefix at s[i] ("Re:", "RE :", "Fw：", "Re[2]:", "Re[3]", "回复："),
// or 0 when there is none.
std::size_t prefix_len_at(std::string_view s, std::size_t i) {
  for (std::string_view tok : kPrefixTokens) {
    if (i + tok.size() > s.size()) continue;
    if (!iequals(s.substr(i, tok.size()), tok)) continue;
    std::size_t j = i + tok.size();
    // An ASCII token must not run into further letters ("Received:", "Fwiw:").
    if (static_cast<unsigned char>(tok[0]) < 0x80 && j < s.size()) {
      const char c = s[j];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
    }
    bool counter = false;
    if (j < s.size() && (s[j] == '[' || s[j] == '(')) {  // Re[2] / Re(2)
      const char close = s[j] == '[' ? ']' : ')';
      std::size_t k = j + 1;
      while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
      if (k > j + 1 && k < s.size() && s[k] == close) {
        counter = true;
        j = k + 1;
      }
    }
    const std::size_t after_ws = skip_spaces(s, j);
    if (after_ws < s.size() && s[after_ws] == ':') return after_ws + 1 - i;
    if (s.substr(after_ws, 3) == "\xEF\xBC\x9A") return after_ws + 3 - i;  // '：'
    if (counter) return j - i;
  }
  return 0;
}

// Length of a "[tag]" list tag at s[i] (non-empty, no nested brackets), or 0.
std::size_t list_tag_len_at(std::string_view s, std::size_t i) {
  if (i >= s.size() || s[i] != '[') return 0;
  const std::size_t close = s.find(']', i + 1);
  if (close == std::string_view::npos || close == i + 1) return 0;
  if (s.substr(i + 1, close - i - 1).find('[') != std::string_view::npos) return 0;
  return close + 1 - i;
}

// Extracts the participant emails (from/to/cc) of the thread's messages.
std::set<std::string> thread_participants(db::Conn& c, int64_t owner, int64_t thread_id) {
  std::set<std::string> out;
  auto s = c.prepare(
      "SELECT from_email, to_json, cc_json FROM messages WHERE thread_id = ? AND owner_id = ?");
  s.bind_all(thread_id, owner);
  while (s.step()) {
    if (const std::string f = normalize_email(s.text(0)); !f.empty()) out.insert(f);
    for (int col : {1, 2})
      for (const auto& a : detail::addresses_from_json(s.text(col)))
        if (std::string e = normalize_email(a.email); !e.empty()) out.insert(std::move(e));
  }
  return out;
}

int64_t create_thread(db::Tx& tx, int64_t owner, std::string_view subject) {
  const int64_t now = azm::now_ms();
  tx.run(
      "INSERT INTO threads(owner_id, subject, norm_subject, created_at, updated_at) "
      "VALUES(?,?,?,?,?)",
      owner, subject, normalize_subject(subject), now, now);
  return tx.last_insert_id();
}

struct MsgRow {
  int64_t id = 0;
  bool out = false;
  bool draft = false;
  std::string from_name, from_email;
  std::string subject, snippet;
  int64_t date = 0;
  bool read = false, starred = false, in_inbox = false, spam = false, trashed = false;
  std::optional<std::string> status;
  std::optional<int64_t> scheduled_at;
};

}  // namespace

std::string normalize_subject(std::string_view subject) {
  const std::string clean = utf8_sanitize(subject);
  const std::string_view s = clean;
  // Leading list tags are kept (they distinguish "[team-a] 周报" from "[team-b] 周报"), but
  // reply/forward prefixes are removed wherever they appear among them:
  //   "Re: [ops] Re: 周报" → "[ops] 周报", "[ops] 回复：周报" → "[ops] 周报".
  std::string tags;
  std::size_t i = skip_spaces(s, 0);
  for (;;) {
    if (const std::size_t p = prefix_len_at(s, i); p > 0) {
      i = skip_spaces(s, i + p);
      continue;
    }
    if (const std::size_t t = list_tag_len_at(s, i); t > 0) {
      // Only a tag that is followed by more subject text is a prefix.
      const std::size_t next = skip_spaces(s, i + t);
      if (next < s.size()) {
        const std::string tag(s.substr(i, t));
        if (tags.find(tag) == std::string::npos) {  // "[ops] Re: [ops] x" → one tag
          if (!tags.empty()) tags.push_back(' ');
          tags += tag;
        }
        i = next;
        continue;
      }
    }
    break;
  }
  std::string rest = tags;
  if (i < s.size()) {
    if (!rest.empty()) rest.push_back(' ');
    rest.append(s.substr(i));
  }
  return to_lower_ascii(detail::collapse_whitespace(rest));
}

bool has_reply_prefix(std::string_view subject) {
  const std::string clean = utf8_sanitize(subject);
  const std::string_view s = clean;
  std::size_t i = skip_spaces(s, 0);
  for (;;) {
    if (prefix_len_at(s, i) > 0) return true;
    if (const std::size_t t = list_tag_len_at(s, i); t > 0) {
      i = skip_spaces(s, i + t);
      continue;
    }
    return false;
  }
}

std::vector<int64_t> threads_for_refs(db::Conn& c, int64_t owner, std::span<const std::string> refs) {
  std::vector<std::string> keys;
  for (const auto& r : refs) {
    std::string k = normalize_message_id(utf8_sanitize(r));
    if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(std::move(k));
    if (keys.size() >= kMaxRefsLookup) break;
  }
  std::vector<int64_t> out;
  if (keys.empty()) return out;
  const std::string list = detail::json_strings(keys);
  auto s = c.prepare(
      "SELECT thread_id FROM messages WHERE owner_id = ?1 AND message_id_header IN "
      "(SELECT value FROM json_each(?2)) UNION SELECT m.thread_id FROM message_refs r "
      "JOIN messages m ON m.id = r.message_id WHERE r.owner_id = ?1 AND m.owner_id = ?1 "
      "AND r.ref IN (SELECT value FROM json_each(?2)) ORDER BY 1");
  s.bind_all(owner, list);
  while (s.step()) out.push_back(s.i64(0));
  return out;
}

int64_t assign_thread(db::Tx& tx, int64_t owner, const ThreadingKeys& keys) {
  // 1. Reference joins (B2): threads holding a referenced Message-ID or sharing a reference,
  //    plus — because the message's own id is part of the lookup — threads whose messages
  //    already reference this message (the reply arrived first). The own id goes first so the
  //    lookup cap never drops it.
  std::vector<std::string> lookup;
  lookup.reserve(keys.refs.size() + 1);
  if (keys.message_id && !keys.message_id->empty()) lookup.push_back(*keys.message_id);
  lookup.insert(lookup.end(), keys.refs.begin(), keys.refs.end());
  std::vector<int64_t> hits = threads_for_refs(tx.conn(), owner, lookup);
  if (!hits.empty()) {
    std::sort(hits.begin(), hits.end());
    const int64_t keep = hits.front();  // the oldest thread survives
    for (std::size_t k = 1; k < hits.size(); ++k) merge_threads(tx, owner, keep, hits[k]);
    return keep;
  }

  // 2. Subject fallback (C7): reply prefix, same normalized subject, recent thread (7 days),
  //    overlapping participants other than the owner's own addresses.
  const std::string norm = normalize_subject(keys.subject);
  if (!norm.empty() && has_reply_prefix(keys.subject)) {
    const std::set<std::string> mine = detail::owner_addresses(tx.conn(), owner);
    std::set<std::string> theirs;
    for (const auto& p : keys.participants) {
      std::string e = normalize_email(p);
      if (!e.empty() && !mine.contains(e)) theirs.insert(std::move(e));
    }
    if (!theirs.empty()) {
      std::vector<int64_t> candidates;
      {
        auto s = tx.prepare(
            "SELECT id FROM threads WHERE owner_id = ? AND norm_subject = ? AND last_at >= ? AND "
            "last_at <= ? ORDER BY last_at DESC, id DESC LIMIT 20");
        s.bind_all(owner, norm, keys.date - kSubjectFallbackWindowMs,
                   keys.date + kSubjectFallbackWindowMs);
        while (s.step()) candidates.push_back(s.i64(0));
      }
      for (int64_t t : candidates) {
        const auto parts = thread_participants(tx.conn(), owner, t);
        const bool overlap = std::any_of(theirs.begin(), theirs.end(),
                                         [&](const std::string& e) { return parts.contains(e); });
        if (overlap) return t;
      }
    }
  }

  // 3. A new, empty thread (recompute_thread fills it once the message is inserted).
  return create_thread(tx, owner, keys.subject);
}

int adopt_referencing(db::Tx& tx, int64_t owner, int64_t thread_id, std::string_view message_id) {
  const std::string id = normalize_message_id(message_id);
  if (id.empty()) return 0;
  std::vector<int64_t> others;
  {
    auto s = tx.prepare(
        "SELECT DISTINCT m.thread_id FROM message_refs r JOIN messages m ON m.id = r.message_id "
        "WHERE r.owner_id = ? AND m.owner_id = ? AND r.ref = ? AND m.thread_id <> ? ORDER BY 1");
    s.bind_all(owner, owner, id, thread_id);
    while (s.step()) others.push_back(s.i64(0));
  }
  for (int64_t t : others) merge_threads(tx, owner, thread_id, t);
  return static_cast<int>(others.size());
}

int64_t merge_threads(db::Tx& tx, int64_t owner, int64_t keep, int64_t absorb) {
  if (keep == absorb) return keep;
  const int64_t owned =
      tx.scalar<int64_t>("SELECT COUNT(*) FROM threads WHERE id IN (?, ?) AND owner_id = ?", keep,
                         absorb, owner)
          .value_or(0);
  if (owned != 2) throw std::invalid_argument("merge_threads: thread not owned by owner");
  tx.run("UPDATE messages SET thread_id = ? WHERE thread_id = ? AND owner_id = ?", keep, absorb, owner);
  tx.run("DELETE FROM threads WHERE id = ? AND owner_id = ?", absorb, owner);
  recompute_thread(tx, owner, keep);
  const std::array<int64_t, 2> ids{keep, absorb};
  tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));
  return keep;
}

bool recompute_thread(db::Tx& tx, int64_t owner, int64_t thread_id) {
  std::vector<MsgRow> rows;
  {
    auto s = tx.prepare(
        "SELECT m.id, m.direction, m.is_draft, m.from_name, m.from_email, m.subject, m.snippet, "
        "m.date, m.is_read, m.is_starred, m.in_inbox, m.is_spam, m.trashed_at, o.status, "
        "o.scheduled_at FROM messages m LEFT JOIN outbound o ON o.id = m.outbound_id "
        "WHERE m.thread_id = ? AND m.owner_id = ? ORDER BY m.date, m.id");
    s.bind_all(thread_id, owner);
    while (s.step()) {
      MsgRow r;
      r.id = s.i64(0);
      r.out = s.text(1) == "out";
      r.draft = s.boolean(2);
      r.from_name = s.text(3);
      r.from_email = s.text(4);
      r.subject = s.text(5);
      r.snippet = s.text(6);
      r.date = s.i64(7);
      r.read = s.boolean(8);
      r.starred = s.boolean(9);
      r.in_inbox = s.boolean(10);
      r.spam = s.boolean(11);
      r.trashed = !s.is_null(12);
      r.status = s.opt_text(13);
      r.scheduled_at = s.opt_i64(14);
      rows.push_back(std::move(r));
    }
  }
  if (rows.empty()) {
    tx.run("DELETE FROM threads WHERE id = ? AND owner_id = ?", thread_id, owner);
    return false;
  }

  int64_t msg_count = 0, unread_count = 0, inbox_count = 0, inbox_unread = 0, starred_count = 0,
          sent_count = 0, draft_count = 0, scheduled_count = 0, spam_count = 0, spam_unread = 0,
          trash_count = 0;
  int64_t last_at = 0, spam_last_at = 0, trash_last_at = 0;
  const MsgRow* latest_normal = nullptr;  // latest normal non-draft (snippet source)
  const MsgRow* latest_draft = nullptr;   // fallbacks for drafts-only / spam / trash threads
  const MsgRow* latest_any = nullptr;

  for (const auto& r : rows) {
    const bool normal = !r.trashed && !r.spam;
    latest_any = &r;  // rows are ascending by (date, id)
    if (normal) {
      last_at = std::max(last_at, r.date);
      if (r.draft) {
        ++draft_count;
        latest_draft = &r;
      } else {
        ++msg_count;
        if (!r.read) ++unread_count;
        latest_normal = &r;
      }
      if (r.in_inbox) {
        ++inbox_count;
        if (!r.read && !r.draft) ++inbox_unread;
      }
      if (r.starred) ++starred_count;
      if (r.out && !r.draft) {
        const std::string st = r.status.value_or("");
        const bool pending = st == "queued" || st == "sending";
        if (st != "scheduled" && st != "canceled" && !(pending && r.scheduled_at)) ++sent_count;
        if (r.scheduled_at && (pending || st == "accepted" || st == "scheduled")) ++scheduled_count;
      }
    } else if (r.trashed) {
      ++trash_count;
      trash_last_at = std::max(trash_last_at, r.date);
    } else {  // spam, not trashed
      ++spam_count;
      if (!r.read && !r.draft) ++spam_unread;
      spam_last_at = std::max(spam_last_at, r.date);
    }
  }

  const MsgRow* snip = latest_normal ? latest_normal : latest_draft ? latest_draft : latest_any;

  // Participants: senders of non-draft messages in order of first appearance; the owner's
  // draft author when the thread has only drafts. When there are more than 6, keep the first
  // one and the 5 most recently active, still in first-appearance order.
  struct P {
    std::string name, email;
    bool unread = false;
    std::size_t first = 0, last = 0;
  };
  std::vector<P> parts;
  std::map<std::string, std::size_t> by_email;
  const bool any_sent = std::any_of(rows.begin(), rows.end(), [](const MsgRow& r) { return !r.draft; });
  for (std::size_t idx = 0; idx < rows.size(); ++idx) {
    const MsgRow& r = rows[idx];
    if (r.draft == any_sent) continue;  // non-drafts when any exist, else drafts
    const std::string key = normalize_email(r.from_email);
    if (key.empty()) continue;
    auto [it, inserted] = by_email.try_emplace(key, parts.size());
    if (inserted) parts.push_back({r.from_name, r.from_email, false, idx, idx});
    P& p = parts[it->second];
    p.last = idx;
    if (!r.from_name.empty()) p.name = r.from_name;
    if (!r.read && !r.draft) p.unread = true;
  }
  if (parts.size() > kMaxParticipants) {
    std::vector<std::size_t> rest(parts.size() - 1);
    for (std::size_t k = 0; k < rest.size(); ++k) rest[k] = k + 1;
    std::sort(rest.begin(), rest.end(), [&](std::size_t a, std::size_t b) { return parts[a].last > parts[b].last; });
    rest.resize(kMaxParticipants - 1);
    std::sort(rest.begin(), rest.end());
    std::vector<P> kept{parts.front()};
    for (std::size_t k : rest) kept.push_back(parts[k]);
    parts = std::move(kept);
  }
  boost::json::array pj;
  for (const auto& p : parts) {
    boost::json::object o;
    o["name"] = p.name;
    o["email"] = p.email;
    o["unread"] = p.unread;
    pj.emplace_back(std::move(o));
  }

  // Non-inline attachments of non-draft messages.
  const int64_t attachment_count =
      tx.scalar<int64_t>(
            "SELECT COUNT(*) FROM attachments a JOIN messages m ON m.id = a.message_id "
            "WHERE m.thread_id = ? AND m.owner_id = ? AND m.is_draft = 0 AND a.is_inline = 0",
            thread_id, owner)
          .value_or(0);

  // Thread subject: the earliest non-draft message's (else the earliest draft's), skipping
  // empty subjects.
  std::string subject;
  for (bool want_draft : {false, true}) {
    for (const auto& r : rows) {
      if (r.draft == want_draft && !r.subject.empty()) {
        subject = r.subject;
        break;
      }
    }
    if (!subject.empty()) break;
  }

  tx.run(
      "UPDATE threads SET subject = ?, norm_subject = ?, last_at = ?, spam_last_at = ?, "
      "trash_last_at = ?, last_message_id = ?, snippet = ?, participants_json = ?, msg_count = ?, "
      "unread_count = ?, inbox_count = ?, inbox_unread = ?, starred_count = ?, sent_count = ?, "
      "draft_count = ?, scheduled_count = ?, spam_count = ?, spam_unread = ?, trash_count = ?, "
      "attachment_count = ?, updated_at = ? WHERE id = ? AND owner_id = ?",
      subject, normalize_subject(subject), last_at, spam_last_at, trash_last_at, snip->id,
      snip->snippet, boost::json::serialize(pj), msg_count, unread_count, inbox_count, inbox_unread,
      starred_count, sent_count, draft_count, scheduled_count, spam_count, spam_unread, trash_count,
      attachment_count, azm::now_ms(), thread_id, owner);
  return true;
}

}  // namespace azm::mail
