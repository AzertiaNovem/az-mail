// Owner: WP-B
// Mailbox reads (thread list, thread/message views, counts, events, contacts) and user actions
// (mailbox.hpp). Every query is scoped to `owner` (IDOR, D6).
#include "mail/mailbox.hpp"

#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/attachments.hpp"
#include "mail/internal.hpp"
#include "mail/outbound.hpp"
#include "mail/render.hpp"
#include "mail/search.hpp"
#include "mail/threads.hpp"
#include "ws/events.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace azm::mail {
namespace {

// ---- cursors ----------------------------------------------------------------------------------

// Opaque keyset cursor: base64url("<sort_key>:<thread_id>") of the last item on the page.
std::string encode_cursor(int64_t key, int64_t id) {
  return crypto::b64url_encode(std::to_string(key) + ":" + std::to_string(id));
}

ApiError bad_cursor() {
  return ApiError::bad_request("invalid_field", "分页参数无效", {{"field", "cursor"}});
}

std::optional<int64_t> to_int(std::string_view s) {
  if (s.empty() || s.size() > 19) return std::nullopt;
  int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

std::pair<int64_t, int64_t> decode_cursor(std::string_view cursor) {
  const auto raw = crypto::b64url_decode(cursor);
  if (!raw) throw bad_cursor();
  const auto colon = raw->find(':');
  if (colon == std::string::npos) throw bad_cursor();
  const auto key = to_int(std::string_view(*raw).substr(0, colon));
  const auto id = to_int(std::string_view(*raw).substr(colon + 1));
  if (!key || !id || *key < 0 || *id <= 0) throw bad_cursor();
  return {*key, *id};
}

// ---- views ------------------------------------------------------------------------------------

// Which message set a list view shows (drives unread / last_at / labels / previews).
enum class Scope { Normal, Spam, Trash, Any };

std::string_view scope_sql(Scope s) {
  switch (s) {
    case Scope::Normal: return "m.trashed_at IS NULL AND m.is_spam = 0";
    case Scope::Spam: return "m.trashed_at IS NULL AND m.is_spam = 1";
    case Scope::Trash: return "m.trashed_at IS NOT NULL";
    case Scope::Any: return "1";
  }
  return "1";
}

// Partial-index predicate (spelled exactly like the index WHERE clause) and sort column.
struct FolderSql {
  std::string_view pred;
  std::string_view key;
};
FolderSql folder_sql(Folder f) {
  switch (f) {
    case Folder::Inbox: return {"inbox_count>0", "last_at"};
    case Folder::Starred: return {"starred_count>0", "last_at"};
    case Folder::Scheduled: return {"scheduled_count>0", "last_at"};
    case Folder::Sent: return {"sent_count>0", "last_at"};
    case Folder::Drafts: return {"draft_count>0", "last_at"};
    case Folder::All: return {"msg_count+draft_count>0", "last_at"};
    case Folder::Spam: return {"spam_count>0", "spam_last_at"};
    case Folder::Trash: return {"trash_count>0", "trash_last_at"};
  }
  return {"inbox_count>0", "last_at"};
}

constexpr std::string_view kThreadCols =
    "id, subject, snippet, participants_json, msg_count, draft_count, unread_count, inbox_count, "
    "inbox_unread, starred_count, spam_unread, attachment_count, last_at, spam_last_at, "
    "trash_last_at";

struct ThreadRow {
  ThreadListItem item;
  int64_t unread_count = 0, inbox_unread = 0, spam_unread = 0;
  int64_t last_at = 0, spam_last_at = 0, trash_last_at = 0;
};

ThreadRow read_thread_row(const db::Stmt& s) {
  ThreadRow r;
  ThreadListItem& t = r.item;
  t.id = s.i64(0);
  t.subject = s.text(1);
  t.snippet = s.text(2);
  {
    for (const auto& el : detail::array_from_json(s.text(3))) {
      const auto* o = el.if_object();
      if (!o) continue;
      Participant p;
      if (const auto* v = o->if_contains("name"); v && v->is_string()) p.name = std::string(v->as_string());
      if (const auto* v = o->if_contains("email"); v && v->is_string()) p.email = std::string(v->as_string());
      if (const auto* v = o->if_contains("unread"); v && v->is_bool()) p.unread = v->as_bool();
      t.participants.push_back(std::move(p));
    }
  }
  t.message_count = s.i64(4);
  t.draft_count = s.i64(5);
  r.unread_count = s.i64(6);
  t.in_inbox = s.i64(7) > 0;
  r.inbox_unread = s.i64(8);
  t.starred = s.i64(9) > 0;
  r.spam_unread = s.i64(10);
  t.has_attachments = s.i64(11) > 0;
  r.last_at = s.i64(12);
  r.spam_last_at = s.i64(13);
  r.trash_last_at = s.i64(14);
  return r;
}

void bind_values(db::Stmt& s, int& idx, const std::vector<db::Value>& vals) {
  for (const auto& v : vals) s.bind(idx++, v);
}

// Fills labels / previews / status / scheduled / unread for a page of threads.
void decorate(db::Conn& c, int64_t owner, std::vector<ThreadRow>& rows, Scope scope,
              std::optional<Folder> folder, const std::vector<int64_t>* search_dates) {
  if (rows.empty()) return;
  std::vector<int64_t> ids;
  std::unordered_map<int64_t, std::size_t> pos;
  for (std::size_t k = 0; k < rows.size(); ++k) {
    ids.push_back(rows[k].item.id);
    pos[rows[k].item.id] = k;
  }
  // One bound JSON array instead of a variable-length IN list (bounded statement cache).
  const std::string ph = "SELECT value FROM json_each(?)";
  const std::string ids_json = detail::json_ids(ids);
  const std::string scope_where(scope_sql(scope));
  auto bind_ids = [&](db::Stmt& s, int start) {
    s.bind(start, ids_json);
    return start + 1;
  };

  {  // label union over the view's messages
    auto s = c.prepare("SELECT DISTINCT m.thread_id, ml.label_id FROM message_labels ml JOIN messages m "
                       "ON m.id = ml.message_id WHERE m.owner_id = ? AND m.thread_id IN (" + ph + ") AND " +
                       scope_where + " ORDER BY m.thread_id, ml.label_id");
    s.bind(1, owner);
    bind_ids(s, 2);
    while (s.step()) rows[pos.at(s.i64(0))].item.label_ids.push_back(s.i64(1));
  }
  {  // first 3 real (non-inline) attachments of sent/received messages
    auto s = c.prepare("SELECT m.thread_id, a.id, a.filename, a.content_type FROM attachments a JOIN messages m "
                       "ON m.id = a.message_id WHERE m.owner_id = ? AND a.owner_id = ? AND m.thread_id IN (" +
                       ph + ") AND a.is_inline = 0 AND m.is_draft = 0 AND " + scope_where +
                       " ORDER BY m.thread_id, m.date, m.id, a.id");
    s.bind(1, owner);
    s.bind(2, owner);
    bind_ids(s, 3);
    while (s.step()) {
      auto& prev = rows[pos.at(s.i64(0))].item.attachments_preview;
      if (prev.size() < 3) prev.push_back({s.i64(1), s.text(2), s.text(3)});
    }
    // The paperclip follows the view's own messages (review R12): the thread aggregate counts the
    // normal set only, so Spam / Trash / search rows decide from the scoped preview instead.
    for (auto& r : rows) r.item.has_attachments = !r.item.attachments_preview.empty();
  }
  {  // status of the latest outbound message
    auto s = c.prepare("SELECT m.thread_id, o.status FROM messages m JOIN outbound o ON o.id = m.outbound_id "
                       "WHERE m.owner_id = ? AND m.thread_id IN (" + ph + ") AND m.is_draft = 0 AND " +
                       scope_where + " ORDER BY m.thread_id, m.date DESC, m.id DESC");
    s.bind(1, owner);
    bind_ids(s, 2);
    std::set<int64_t> seen;
    while (s.step()) {
      const int64_t tid = s.i64(0);
      if (!seen.insert(tid).second) continue;
      rows[pos.at(tid)].item.latest_status = parse_outbound_status(s.text(1));
    }
  }
  {  // recipients of the newest outbound message (Sent / Scheduled rows: "收件人：…", F16). To + Cc
     // only — never Bcc — of the owner's own out messages in the view (owner-scoped).
    constexpr std::size_t kMaxToPreview = 3;
    auto s = c.prepare("SELECT m.thread_id, m.to_json, m.cc_json FROM messages m WHERE m.owner_id = ? AND "
                       "m.thread_id IN (" + ph + ") AND m.is_draft = 0 AND m.direction = 'out' AND " +
                       scope_where + " ORDER BY m.thread_id, m.date DESC, m.id DESC");
    s.bind(1, owner);
    bind_ids(s, 2);
    std::set<int64_t> seen;
    while (s.step()) {
      const int64_t tid = s.i64(0);
      if (!seen.insert(tid).second) continue;
      auto& out = rows[pos.at(tid)].item.to_preview;
      std::set<std::string> emails;
      for (const auto& list : {detail::addresses_from_json(s.text(1)), detail::addresses_from_json(s.text(2))}) {
        for (const auto& a : list) {
          if (out.size() >= kMaxToPreview) break;
          if (!emails.insert(normalize_email(a.email)).second) continue;
          Participant p;
          p.name = a.name;
          p.email = a.email;
          out.push_back(std::move(p));
        }
      }
    }
  }
  {  // earliest pending schedule (same rule as scheduled_count)
    auto s = c.prepare(
        "SELECT m.thread_id, MIN(o.scheduled_at) FROM messages m JOIN outbound o ON o.id = m.outbound_id "
        "WHERE m.owner_id = ? AND m.thread_id IN (" + ph + ") AND m.is_draft = 0 AND m.direction = 'out' "
        "AND m.trashed_at IS NULL AND m.is_spam = 0 AND o.scheduled_at IS NOT NULL AND o.status IN "
        "('queued','sending','accepted','scheduled') GROUP BY m.thread_id");
    s.bind(1, owner);
    bind_ids(s, 2);
    while (s.step()) rows[pos.at(s.i64(0))].item.scheduled_at = s.opt_i64(1);
  }

  // Unread is folder-aware; trash has no aggregate, so it is counted here.
  const bool need_trash_unread = folder == Folder::Trash || (!folder && scope == Scope::Any);
  std::set<int64_t> trash_unread;
  if (need_trash_unread) {
    auto s = c.prepare("SELECT DISTINCT thread_id FROM messages WHERE owner_id = ? AND thread_id IN (" + ph +
                       ") AND trashed_at IS NOT NULL AND is_read = 0 AND is_draft = 0");
    s.bind(1, owner);
    bind_ids(s, 2);
    while (s.step()) trash_unread.insert(s.i64(0));
  }
  for (std::size_t k = 0; k < rows.size(); ++k) {
    ThreadRow& r = rows[k];
    ThreadListItem& t = r.item;
    const bool tu = trash_unread.contains(t.id);
    if (folder == Folder::Inbox) t.unread = r.inbox_unread > 0;
    else if (folder == Folder::Spam) t.unread = r.spam_unread > 0;
    else if (folder == Folder::Trash) t.unread = tu;
    else if (!folder && scope == Scope::Any) t.unread = r.unread_count > 0 || r.spam_unread > 0 || tu;
    else t.unread = r.unread_count > 0;

    if (search_dates) t.last_at = (*search_dates)[k];
    else if (folder == Folder::Spam) t.last_at = r.spam_last_at;
    else if (folder == Folder::Trash) t.last_at = r.trash_last_at;
    else t.last_at = r.last_at;
  }
}

void mark_me(std::vector<ThreadRow>& rows, const std::set<std::string>& mine) {
  for (auto& r : rows) {
    for (auto& p : r.item.participants) p.is_me = mine.contains(normalize_email(p.email));
    for (auto& p : r.item.to_preview) p.is_me = mine.contains(normalize_email(p.email));
  }
}

// ---- message views ----------------------------------------------------------------------------

constexpr std::string_view kMessageSelect =
    "SELECT m.id, m.thread_id, m.direction, m.is_draft, m.from_name, m.from_email, m.to_json, "
    "m.cc_json, m.bcc_json, m.reply_to_json, m.delivered_to, m.subject, m.snippet, m.date, b.html, "
    "b.text, m.is_read, m.is_starred, m.in_inbox, m.is_spam, m.trashed_at, m.auth_spf, m.auth_dkim, "
    "m.auth_dmarc, m.warnings_json, o.id, o.status, o.status_detail, o.scheduled_at, o.scheduled_via, "
    "o.send_after, COALESCE(o.accepted_at, (SELECT MIN(de.occurred_at) FROM delivery_events de "
    "WHERE de.outbound_id = o.id AND de.type = 'email.sent')), m.message_id_header, i.raw_sha256, "
    "m.is_shared_copy, u.email, u.display_name "
    "FROM messages m LEFT JOIN message_bodies b ON b.message_id = m.id "
    "LEFT JOIN outbound o ON o.id = m.outbound_id "
    "LEFT JOIN inbound_emails i ON i.id = m.inbound_id "
    "LEFT JOIN users u ON u.id = m.sent_by_user_id ";

MessageView read_message(const db::Stmt& s, const SignedUrls& urls, int64_t owner, int64_t exp) {
  MessageView v;
  v.id = s.i64(0);
  v.thread_id = s.i64(1);
  v.direction = parse_direction(s.text(2)).value_or(Direction::In);
  v.is_draft = s.boolean(3);
  v.from = Address{s.text(4), s.text(5)};
  v.to = detail::addresses_from_json(s.text(6));
  v.cc = detail::addresses_from_json(s.text(7));
  v.bcc = detail::addresses_from_json(s.text(8));
  v.reply_to = detail::addresses_from_json(s.text(9));
  v.delivered_to = s.opt_text(10);
  v.subject = s.text(11);
  v.snippet = s.text(12);
  v.date = s.i64(13);
  v.html = s.opt_text(14);
  v.text = s.opt_text(15);
  v.is_read = s.boolean(16);
  v.is_starred = s.boolean(17);
  v.in_inbox = s.boolean(18);
  v.is_spam = s.boolean(19);
  v.trashed = !s.is_null(20);
  if (v.direction == Direction::In && !v.is_draft)
    v.auth = AuthResults{s.opt_text(21), s.opt_text(22), s.opt_text(23)};
  v.warnings = detail::strings_from_json(s.text(24));
  if (!s.is_null(25) && !v.is_draft) {
    OutboundView o;
    o.id = s.i64(25);
    o.status = parse_outbound_status(s.text(26)).value_or(OutboundStatus::Queued);
    o.status_detail = s.opt_text(27);
    o.scheduled_at = s.opt_i64(28);
    if (auto via = s.opt_text(29)) o.scheduled_via = parse_scheduled_via(*via);
    if (o.status == OutboundStatus::Queued && !o.scheduled_at) o.undo_until = s.opt_i64(30);
    o.sent_at = s.opt_i64(31);
    v.outbound = o;
  }
  v.message_id_header = s.opt_text(32);
  if (v.direction == Direction::In && !s.is_null(33)) v.raw_url = urls.raw_url(v.id, owner, exp);
  if (s.boolean(34) && !s.is_null(35)) v.sent_by = Address{s.text(36), s.text(35)};
  return v;
}

// Loads views (bodies rewritten, attachments signed, labels) of one message or of a whole
// thread, ascending by (date, id).
enum class Load { Message, Thread };

std::vector<MessageView> load_messages(db::Conn& c, const SignedUrls& urls, int64_t owner, Load what,
                                       int64_t id) {
  const std::string where = what == Load::Thread ? "m.thread_id = ?" : "m.id = ?";
  const int64_t exp = urls.expiry(azm::now_ms());
  std::vector<MessageView> out;
  {
    auto s = c.prepare(std::string(kMessageSelect) + "WHERE m.owner_id = ? AND " + where + " ORDER BY m.date, m.id");
    s.bind_all(owner, id);
    while (s.step()) out.push_back(read_message(s, urls, owner, exp));
  }
  if (out.empty()) return out;

  std::unordered_map<int64_t, std::size_t> pos;
  for (std::size_t k = 0; k < out.size(); ++k) pos[out[k].id] = k;
  {
    auto s = c.prepare("SELECT ml.message_id, ml.label_id FROM message_labels ml JOIN messages m ON "
                       "m.id = ml.message_id WHERE m.owner_id = ? AND " + where +
                       " ORDER BY ml.message_id, ml.label_id");
    s.bind_all(owner, id);
    while (s.step()) {
      if (auto it = pos.find(s.i64(0)); it != pos.end()) out[it->second].label_ids.push_back(s.i64(1));
    }
  }
  const std::vector<AttachmentRecord> atts =
      what == Load::Thread ? thread_attachments(c, owner, id) : message_attachments(c, owner, id);
  std::unordered_map<int64_t, std::vector<CidTarget>> targets;
  for (const auto& r : atts) {
    const auto it = pos.find(r.message_id.value_or(0));
    if (it == pos.end()) continue;
    if (r.content_id && !r.content_id->empty()) targets[*r.message_id].push_back({r.id, *r.content_id});
    out[it->second].attachments.push_back(make_attachment_view(r, urls, owner, exp));
  }
  // Stored HTML references attachments as cid: (C10); signed URLs exist only in responses.
  for (auto& v : out) {
    if (!v.html) continue;
    if (auto it = targets.find(v.id); it != targets.end())
      v.html = rewrite_cid_to_signed(*v.html, it->second, urls, owner, exp);
  }
  return out;
}

void require_label(db::Conn& c, int64_t owner, int64_t label_id) {
  if (!c.scalar<int64_t>("SELECT 1 FROM labels WHERE id = ? AND owner_id = ?", label_id, owner))
    throw ApiError::not_found("not_found", "标签不存在");
}

int64_t clamp_limit(int limit, int lo, int hi) { return std::clamp(limit, lo, hi); }

}  // namespace

// =============================================================================================
// Thread list
// =============================================================================================

ThreadPage list_threads(db::Conn& c, int64_t owner, const ThreadQuery& q) {
  const int64_t limit = clamp_limit(q.limit, 1, 100);
  const int64_t now = q.now_ms != 0 ? q.now_ms : azm::now_ms();
  std::optional<std::pair<int64_t, int64_t>> cursor;
  if (q.cursor && !q.cursor->empty()) cursor = decode_cursor(*q.cursor);

  ThreadPage page;
  std::vector<ThreadRow> rows;
  std::vector<int64_t> sort_keys;  // key of each row (cursor source)
  Scope scope = Scope::Normal;
  std::optional<Folder> folder;
  bool is_search = false;
  std::vector<int64_t> search_dates;

  if (q.q && !trim(*q.q).empty()) {
    // Search: message-level filter grouped by thread, ordered by the latest matching date.
    is_search = true;
    const SqlFilter f = compile_search(*q.q, q.tzoff_min, now);
    folder = f.in_folder;
    if (f.in_folder == Folder::Spam) scope = Scope::Spam;
    else if (f.in_folder == Folder::Trash) scope = Scope::Trash;
    else if (f.include_spam_trash) scope = Scope::Any;
    std::string sql = "SELECT m.thread_id, MAX(m.date) AS d FROM messages m WHERE m.owner_id = ? AND (" +
                      f.where + ") GROUP BY m.thread_id";
    if (cursor) sql += " HAVING (d < ? OR (d = ? AND m.thread_id < ?))";
    sql += " ORDER BY d DESC, m.thread_id DESC LIMIT ?";
    auto s = c.prepare(sql);
    int idx = 1;
    s.bind(idx++, owner);
    bind_values(s, idx, f.binds);
    if (cursor) {
      s.bind(idx++, cursor->first);
      s.bind(idx++, cursor->first);
      s.bind(idx++, cursor->second);
    }
    s.bind(idx++, limit + 1);
    std::vector<std::pair<int64_t, int64_t>> hits;
    while (s.step()) hits.emplace_back(s.i64(0), s.i64(1));
    if (static_cast<int64_t>(hits.size()) > limit) {
      hits.resize(static_cast<std::size_t>(limit));
      page.next_cursor = encode_cursor(hits.back().second, hits.back().first);
    }
    if (!hits.empty()) {
      std::unordered_map<int64_t, ThreadRow> by_id;
      auto t = c.prepare("SELECT " + std::string(kThreadCols) +
                         " FROM threads WHERE owner_id = ? AND id IN (SELECT value FROM json_each(?))");
      std::vector<int64_t> hit_ids;
      hit_ids.reserve(hits.size());
      for (const auto& h : hits) hit_ids.push_back(h.first);
      t.bind_all(owner, detail::json_ids(hit_ids));
      while (t.step()) {
        ThreadRow r = read_thread_row(t);
        by_id.emplace(r.item.id, std::move(r));
      }
      for (const auto& h : hits) {
        auto it = by_id.find(h.first);
        if (it == by_id.end()) continue;
        rows.push_back(std::move(it->second));
        search_dates.push_back(h.second);
      }
    }
  } else if (q.label_id) {
    require_label(c, owner, *q.label_id);
    const std::string in_label =
        "id IN (SELECT m.thread_id FROM message_labels ml JOIN messages m ON m.id = ml.message_id "
        "WHERE ml.label_id = ? AND m.owner_id = ? AND m.trashed_at IS NULL AND m.is_spam = 0)";
    std::string sql = "SELECT " + std::string(kThreadCols) + " FROM threads WHERE owner_id = ? AND " + in_label;
    if (cursor) sql += " AND (last_at, id) < (?, ?)";
    sql += " ORDER BY last_at DESC, id DESC LIMIT ?";
    auto s = c.prepare(sql);
    int idx = 1;
    s.bind(idx++, owner);
    s.bind(idx++, *q.label_id);
    s.bind(idx++, owner);
    if (cursor) {
      s.bind(idx++, cursor->first);
      s.bind(idx++, cursor->second);
    }
    s.bind(idx++, limit + 1);
    while (s.step()) {
      rows.push_back(read_thread_row(s));
      sort_keys.push_back(rows.back().last_at);
    }
    page.total = c.scalar<int64_t>("SELECT COUNT(*) FROM threads WHERE owner_id = ? AND " + in_label, owner,
                                   *q.label_id, owner)
                     .value_or(0);
  } else {
    folder = q.folder.value_or(Folder::Inbox);
    if (folder == Folder::Spam) scope = Scope::Spam;
    else if (folder == Folder::Trash) scope = Scope::Trash;
    const FolderSql fs = folder_sql(*folder);
    std::string sql = "SELECT " + std::string(kThreadCols) + " FROM threads WHERE owner_id = ? AND " +
                      std::string(fs.pred);
    if (cursor) sql += " AND (" + std::string(fs.key) + ", id) < (?, ?)";
    sql += " ORDER BY " + std::string(fs.key) + " DESC, id DESC LIMIT ?";
    auto s = c.prepare(sql);
    int idx = 1;
    s.bind(idx++, owner);
    if (cursor) {
      s.bind(idx++, cursor->first);
      s.bind(idx++, cursor->second);
    }
    s.bind(idx++, limit + 1);
    while (s.step()) {
      rows.push_back(read_thread_row(s));
      const ThreadRow& r = rows.back();
      sort_keys.push_back(*folder == Folder::Spam    ? r.spam_last_at
                          : *folder == Folder::Trash ? r.trash_last_at
                                                     : r.last_at);
    }
    page.total = c.scalar<int64_t>("SELECT COUNT(*) FROM threads WHERE owner_id = ? AND " + std::string(fs.pred),
                                   owner)
                     .value_or(0);
  }

  if (!is_search && static_cast<int64_t>(rows.size()) > limit) {
    rows.resize(static_cast<std::size_t>(limit));
    page.next_cursor = encode_cursor(sort_keys[rows.size() - 1], rows.back().item.id);
  }

  decorate(c, owner, rows, scope, folder, is_search ? &search_dates : nullptr);
  mark_me(rows, detail::owner_addresses(c, owner));
  page.items.reserve(rows.size());
  for (auto& r : rows) page.items.push_back(std::move(r.item));
  return page;
}

// =============================================================================================
// Thread / message views
// =============================================================================================

std::optional<ThreadDetail> get_thread(db::Conn& c, const SignedUrls& urls, int64_t owner,
                                       int64_t thread_id) {
  auto s = c.prepare("SELECT id, subject FROM threads WHERE id = ? AND owner_id = ?");
  s.bind_all(thread_id, owner);
  if (!s.step()) return std::nullopt;
  ThreadDetail d;
  d.id = s.i64(0);
  d.subject = s.text(1);
  d.messages = load_messages(c, urls, owner, Load::Thread, thread_id);
  std::set<int64_t> labels;
  for (const auto& m : d.messages) labels.insert(m.label_ids.begin(), m.label_ids.end());
  d.label_ids.assign(labels.begin(), labels.end());
  return d;
}

std::optional<MessageView> get_message(db::Conn& c, const SignedUrls& urls, int64_t owner,
                                       int64_t message_id) {
  auto v = load_messages(c, urls, owner, Load::Message, message_id);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}

std::vector<int64_t> message_label_ids(db::Conn& c, int64_t owner, int64_t message_id) {
  std::vector<int64_t> out;
  auto s = c.prepare(
      "SELECT ml.label_id FROM message_labels ml JOIN messages m ON m.id = ml.message_id "
      "WHERE m.id = ? AND m.owner_id = ? ORDER BY ml.label_id");
  s.bind_all(message_id, owner);
  while (s.step()) out.push_back(s.i64(0));
  return out;
}

std::optional<std::vector<DeliveryEventView>> message_events(db::Conn& c, int64_t owner,
                                                             int64_t message_id) {
  auto m = c.prepare("SELECT outbound_id FROM messages WHERE id = ? AND owner_id = ?");
  m.bind_all(message_id, owner);
  if (!m.step()) return std::nullopt;
  std::vector<DeliveryEventView> out;
  const auto outbound_id = m.opt_i64(0);
  if (!outbound_id) return out;
  auto s = c.prepare(
      "SELECT type, occurred_at, detail_json FROM delivery_events WHERE outbound_id = ? "
      "ORDER BY occurred_at, id");
  s.bind_all(*outbound_id);
  while (s.step()) out.push_back({s.text(0), s.i64(1), detail::object_from_json(s.text(2))});
  return out;
}

// =============================================================================================
// Counts
// =============================================================================================

Counts counts(db::Conn& c, int64_t owner) {
  Counts out;
  // Predicates repeat the partial-index conditions so each count is an index scan.
  out.inbox_unread = c.scalar<int64_t>(
                          "SELECT COUNT(*) FROM threads WHERE owner_id = ? AND inbox_count>0 AND inbox_unread > 0",
                          owner)
                         .value_or(0);
  out.drafts = c.scalar<int64_t>("SELECT COUNT(*) FROM threads WHERE owner_id = ? AND draft_count>0", owner)
                   .value_or(0);
  out.scheduled =
      c.scalar<int64_t>("SELECT COUNT(*) FROM threads WHERE owner_id = ? AND scheduled_count>0", owner)
          .value_or(0);
  out.spam_unread = c.scalar<int64_t>(
                         "SELECT COUNT(*) FROM threads WHERE owner_id = ? AND spam_count>0 AND spam_unread > 0",
                         owner)
                        .value_or(0);
  auto s = c.prepare(
      "SELECT l.id, "
      "(SELECT COUNT(DISTINCT m.thread_id) FROM message_labels ml JOIN messages m ON m.id = ml.message_id "
      " WHERE ml.label_id = l.id AND m.owner_id = l.owner_id AND m.trashed_at IS NULL AND m.is_spam = 0 "
      " AND m.is_draft = 0 AND m.is_read = 0), "
      "(SELECT COUNT(DISTINCT m.thread_id) FROM message_labels ml JOIN messages m ON m.id = ml.message_id "
      " WHERE ml.label_id = l.id AND m.owner_id = l.owner_id AND m.trashed_at IS NULL AND m.is_spam = 0) "
      "FROM labels l WHERE l.owner_id = ? ORDER BY l.id");
  s.bind_all(owner);
  while (s.step()) out.labels[s.i64(0)] = LabelCount{s.i64(1), s.i64(2)};
  return out;
}

// =============================================================================================
// Actions
// =============================================================================================

namespace {

// Latest message to star: latest normal non-draft, else latest non-draft, else latest.
std::optional<int64_t> star_target(db::Tx& tx, int64_t owner, int64_t thread_id) {
  for (std::string_view cond : {"is_draft = 0 AND trashed_at IS NULL AND is_spam = 0", "is_draft = 0", "1"}) {
    if (auto id = tx.scalar<int64_t>("SELECT id FROM messages WHERE thread_id = ? AND owner_id = ? AND " +
                                         std::string(cond) + " ORDER BY date DESC, id DESC LIMIT 1",
                                     thread_id, owner))
      return id;
  }
  return std::nullopt;
}

// Review R2: the sender's own copies of pending sends in the thread are settled before the copies
// are trashed or deleted — never may a send go out from a copy the user removed (deleting it
// would also cascade its attachments away, and nobody could cancel it any more). Queued ones are
// canceled in this transaction; ones Resend holds (or that are being POSTed) are refused.
void settle_pending_sends(db::Tx& tx, int64_t owner, int64_t tid, CopyRemoval how, int64_t now) {
  std::vector<int64_t> copies;
  {
    auto s = tx.prepare(
        std::string("SELECT id FROM messages WHERE thread_id = ? AND owner_id = ? AND is_shared_copy = 0 AND "
                    "is_draft = 0 AND outbound_id IS NOT NULL AND ") +
        (how == CopyRemoval::Trash ? "trashed_at IS NULL" : "(trashed_at IS NOT NULL OR is_spam = 1)") +
        " ORDER BY id");
    s.bind_all(tid, owner);
    while (s.step()) copies.push_back(s.i64(0));
  }
  for (const int64_t id : copies) {
    switch (cancel_pending_send(tx, owner, id, how, now)) {
      case PendingSend::RemoteScheduled:
        throw ApiError::conflict("scheduled_send_pending", "该邮件已定时发送，请先取消定时再删除",
                                 {{"thread_id", tid}, {"message_id", id}});
      case PendingSend::InFlight:
        if (how == CopyRemoval::Trash)
          throw ApiError::conflict("scheduled_send_pending", "该定时邮件正在提交，请稍后再试",
                                   {{"thread_id", tid}, {"message_id", id}});
        throw ApiError::conflict("send_in_progress", "邮件正在发送，请稍后再删除",
                                 {{"thread_id", tid}, {"message_id", id}});
      case PendingSend::None:
      case PendingSend::Canceled: break;
    }
  }
}

void apply_to_thread(db::Tx& tx, int64_t owner, int64_t tid, ThreadAction action,
                     std::optional<int64_t> label_id, int64_t now) {
  constexpr std::string_view kThread = " WHERE thread_id = ? AND owner_id = ?";
  auto run = [&](std::string_view set_and_where, auto&&... extra) {
    tx.run("UPDATE messages SET " + std::string(set_and_where), now, extra...);
  };
  switch (action) {
    case ThreadAction::Archive:
      run("in_inbox = 0, updated_at = ?" + std::string(kThread) + " AND in_inbox = 1", tid, owner);
      break;
    case ThreadAction::Inbox: {
      // Move to inbox: out of spam/trash, and back into the inbox (received messages; the sent
      // ones when the thread has nothing received).
      run("is_spam = 0, trashed_at = NULL, updated_at = ?" + std::string(kThread) +
              " AND (is_spam = 1 OR trashed_at IS NOT NULL)",
          tid, owner);
      const bool has_in = tx.scalar<int64_t>("SELECT 1 FROM messages" + std::string(kThread) +
                                                 " AND direction = 'in' AND is_draft = 0 LIMIT 1",
                                             tid, owner)
                              .has_value();
      run("in_inbox = 1, updated_at = ?" + std::string(kThread) + " AND is_draft = 0 AND in_inbox = 0" +
              (has_in ? " AND direction = 'in'" : ""),
          tid, owner);
      break;
    }
    case ThreadAction::Read:
      run("is_read = 1, updated_at = ?" + std::string(kThread) + " AND is_draft = 0 AND is_read = 0", tid, owner);
      break;
    case ThreadAction::Unread:
      run("is_read = 0, updated_at = ?" + std::string(kThread) + " AND is_draft = 0 AND is_read = 1", tid, owner);
      break;
    case ThreadAction::Star:
      if (auto id = star_target(tx, owner, tid))
        run("is_starred = 1, updated_at = ? WHERE id = ? AND owner_id = ?", *id, owner);
      break;
    case ThreadAction::Unstar:
      run("is_starred = 0, updated_at = ?" + std::string(kThread) + " AND is_starred = 1", tid, owner);
      break;
    case ThreadAction::Trash:
      settle_pending_sends(tx, owner, tid, CopyRemoval::Trash, now);
      tx.run("UPDATE messages SET trashed_at = ?, updated_at = ?" + std::string(kThread) +
                 " AND trashed_at IS NULL",
             now, now, tid, owner);
      break;
    case ThreadAction::Restore:
      run("trashed_at = NULL, updated_at = ?" + std::string(kThread) + " AND trashed_at IS NOT NULL", tid, owner);
      break;
    case ThreadAction::Spam:
      run("is_spam = 1, in_inbox = 0, updated_at = ?" + std::string(kThread) + " AND is_draft = 0", tid, owner);
      break;
    case ThreadAction::NotSpam:
      run("is_spam = 0, updated_at = ?" + std::string(kThread) + " AND is_spam = 1", tid, owner);
      run("in_inbox = 1, updated_at = ?" + std::string(kThread) +
              " AND direction = 'in' AND is_draft = 0 AND trashed_at IS NULL",
          tid, owner);
      break;
    case ThreadAction::DeleteForever:
      settle_pending_sends(tx, owner, tid, CopyRemoval::Delete, now);
      tx.run("DELETE FROM messages" + std::string(kThread) + " AND (trashed_at IS NOT NULL OR is_spam = 1)", tid,
             owner);
      break;
    case ThreadAction::AddLabel:
      tx.run("INSERT OR IGNORE INTO message_labels(message_id, label_id) SELECT id, ? FROM messages" +
                 std::string(kThread) + " AND trashed_at IS NULL AND is_spam = 0",
             *label_id, tid, owner);
      break;
    case ThreadAction::RemoveLabel:
      tx.run("DELETE FROM message_labels WHERE label_id = ? AND message_id IN (SELECT id FROM messages" +
                 std::string(kThread) + ")",
             *label_id, tid, owner);
      break;
  }
}

}  // namespace

std::vector<int64_t> apply_thread_action(db::Tx& tx, int64_t owner, std::span<const int64_t> ids,
                                         ThreadAction action, std::optional<int64_t> label_id) {
  if (needs_label(action)) {
    if (!label_id) throw ApiError::bad_request("invalid_field", "缺少标签", {{"field", "label_id"}});
    require_label(tx.conn(), owner, *label_id);
  }
  const int64_t now = azm::now_ms();
  std::vector<int64_t> affected;
  std::set<int64_t> seen;
  for (int64_t tid : ids) {
    if (!seen.insert(tid).second) continue;
    if (!tx.scalar<int64_t>("SELECT 1 FROM threads WHERE id = ? AND owner_id = ?", tid, owner)) continue;
    apply_to_thread(tx, owner, tid, action, label_id, now);
    recompute_thread(tx, owner, tid);
    affected.push_back(tid);
  }
  if (!affected.empty())
    tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(affected));
  return affected;
}

void patch_message(db::Tx& tx, int64_t owner, int64_t message_id, const MessagePatch& patch) {
  const auto thread_id =
      tx.scalar<int64_t>("SELECT thread_id FROM messages WHERE id = ? AND owner_id = ?", message_id, owner);
  if (!thread_id) throw ApiError::not_found("not_found", "邮件不存在");
  for (int64_t l : patch.add_label_ids) require_label(tx.conn(), owner, l);
  for (int64_t l : patch.remove_label_ids) require_label(tx.conn(), owner, l);

  const int64_t now = azm::now_ms();
  if (patch.is_read)
    tx.run("UPDATE messages SET is_read = ?, updated_at = ? WHERE id = ? AND owner_id = ?", *patch.is_read, now,
           message_id, owner);
  if (patch.is_starred)
    tx.run("UPDATE messages SET is_starred = ?, updated_at = ? WHERE id = ? AND owner_id = ?", *patch.is_starred,
           now, message_id, owner);
  for (int64_t l : patch.add_label_ids)
    tx.run("INSERT OR IGNORE INTO message_labels(message_id, label_id) VALUES(?, ?)", message_id, l);
  for (int64_t l : patch.remove_label_ids)
    tx.run("DELETE FROM message_labels WHERE message_id = ? AND label_id = ?", message_id, l);

  recompute_thread(tx, owner, *thread_id);
  const std::array<int64_t, 1> ids{*thread_id};
  tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));
}

// =============================================================================================
// Contacts
// =============================================================================================

std::vector<ContactView> search_contacts(db::Conn& c, int64_t owner, std::string_view q_in, int limit) {
  const int64_t lim = clamp_limit(limit, 1, 50);
  const std::string q(trim(q_in));
  const std::string pat = detail::like_contains(q);
  std::vector<ContactView> out;
  std::set<std::string> seen;
  auto add = [&](std::string email, std::string name, ContactKind kind) {
    if (static_cast<int64_t>(out.size()) >= lim) return;
    if (!seen.insert(normalize_email(email)).second) return;
    out.push_back({std::move(name), std::move(email), kind});
  };

  // Team directory entries (never stored in `contacts`) rank prefix matches first:
  // "zh" → "zhang@…" before "lizhen@…", "张" → "张伟".
  const std::string prefix = detail::like_contains(q).substr(1);  // escaped "q%"
  {  // team members: active users' mailboxes
    auto s = c.prepare(
        "SELECT a.email, CASE WHEN a.display_name <> '' THEN a.display_name ELSE u.display_name END AS n "
        "FROM addresses a JOIN users u ON u.id = a.user_id "
        "WHERE a.kind = 'user' AND u.disabled = 0 AND (a.email LIKE ?1 ESCAPE '\\' OR "
        "a.display_name LIKE ?1 ESCAPE '\\' OR u.display_name LIKE ?1 ESCAPE '\\') "
        "ORDER BY (a.email LIKE ?2 ESCAPE '\\' OR n LIKE ?2 ESCAPE '\\') DESC, a.email LIMIT ?3");
    s.bind_all(pat, prefix, lim);
    while (s.step()) add(s.text(0), s.text(1), ContactKind::Team);
  }
  {  // aliases
    auto s = c.prepare(
        "SELECT email, display_name FROM addresses WHERE kind = 'alias' AND (email LIKE ?1 ESCAPE '\\' OR "
        "display_name LIKE ?1 ESCAPE '\\') ORDER BY (email LIKE ?2 ESCAPE '\\' OR display_name LIKE ?2 "
        "ESCAPE '\\') DESC, email LIMIT ?3");
    s.bind_all(pat, prefix, lim);
    while (s.step()) add(s.text(0), s.text(1), ContactKind::Alias);
  }
  {
    auto s = c.prepare(
        "SELECT email, name FROM contacts WHERE owner_id = ? AND (email LIKE ? ESCAPE '\\' OR "
        "name LIKE ? ESCAPE '\\') ORDER BY score DESC, last_used_at DESC, email LIMIT ?");
    s.bind_all(owner, pat, pat, lim + static_cast<int64_t>(out.size()));
    while (s.step()) add(s.text(0), s.text(1), ContactKind::Contact);
  }
  return out;
}

void upsert_contact(db::Tx& tx, int64_t owner, const Address& addr, double score_delta, int64_t now_ms) {
  const std::string email = normalize_email(addr.email);
  if (!is_valid_email(email)) return;
  if (tx.scalar<int64_t>("SELECT 1 FROM addresses WHERE email = ?", email)) return;  // team directory
  const std::string name(trim(addr.name));
  tx.run(
      "INSERT INTO contacts(owner_id, email, name, score, last_used_at) VALUES(?,?,?,?,?) "
      "ON CONFLICT(owner_id, email) DO UPDATE SET score = score + excluded.score, "
      "last_used_at = MAX(last_used_at, excluded.last_used_at), "
      "name = CASE WHEN excluded.name <> '' THEN excluded.name ELSE name END",
      owner, email, name, score_delta, now_ms);
}

// =============================================================================================
// Retention
// =============================================================================================

PurgeResult purge_trash(db::Tx& tx, int64_t now_ms, int trash_days, int spam_days, int limit) {
  PurgeResult res;
  if (limit <= 0) return res;
  constexpr int64_t kDay = 24LL * 3600 * 1000;
  struct Victim {
    int64_t id, owner, thread;
  };
  std::vector<Victim> victims;
  {
    // Negative retention disables that half; 0 purges immediately. Each half is a scan of
    // its partial index (messages_trashed / messages_spam).
    // The sender's copy of a send Resend holds (scheduled) or that is being POSTed is kept until
    // that settles (review R2); queued sends are canceled below before their copy goes.
    constexpr std::string_view kHeld =
        " AND NOT (is_shared_copy = 0 AND outbound_id IS NOT NULL AND EXISTS (SELECT 1 FROM outbound o WHERE "
        "o.id = messages.outbound_id AND (o.status = 'sending' OR (o.status IN ('accepted','scheduled') AND "
        "o.scheduled_via = 'resend' AND o.scheduled_at > ?))))";
    std::vector<std::string> parts;
    std::vector<int64_t> binds;
    if (trash_days >= 0) {
      parts.emplace_back("SELECT id, owner_id, thread_id FROM messages WHERE trashed_at IS NOT NULL AND trashed_at <= ?" +
                         std::string(kHeld));
      binds.push_back(now_ms - trash_days * kDay);
      binds.push_back(now_ms);
    }
    if (spam_days >= 0) {
      // Spam retention counts from when the copy became spam, not from its (sender-controlled,
      // possibly old) Date header (review R5): delivered as spam → created_at; reported as spam
      // → the Spam action bumped updated_at. Both only ever lengthen retention.
      parts.emplace_back(
          "SELECT id, owner_id, thread_id FROM messages WHERE is_spam = 1 AND MAX(date, created_at, updated_at) <= ?" +
          std::string(kHeld));
      binds.push_back(now_ms - spam_days * kDay);
      binds.push_back(now_ms);
    }
    if (parts.empty()) return res;
    std::string sql = join(parts, " UNION ") + " ORDER BY 1 LIMIT ?";
    auto s = tx.prepare(sql);
    int idx = 1;
    for (int64_t b : binds) s.bind(idx++, b);
    s.bind(idx++, static_cast<int64_t>(limit) + 1);
    while (s.step()) victims.push_back({s.i64(0), s.i64(1), s.i64(2)});
  }
  if (static_cast<int>(victims.size()) > limit) {
    res.more = true;
    victims.resize(static_cast<std::size_t>(limit));
  }
  std::map<int64_t, std::set<int64_t>> threads_by_owner;
  for (const auto& v : victims) {
    // A queued send of this (sender) copy is canceled first: it must not go out without its copy
    // and attachments (review R2).
    const PendingSend pending = cancel_pending_send(tx, v.owner, v.id, CopyRemoval::Delete, now_ms);
    if (pending == PendingSend::RemoteScheduled || pending == PendingSend::InFlight) continue;
    tx.run("DELETE FROM messages WHERE id = ?", v.id);
    threads_by_owner[v.owner].insert(v.thread);
    ++res.messages_deleted;
  }
  for (const auto& [owner, threads] : threads_by_owner) {
    std::vector<int64_t> ids(threads.begin(), threads.end());
    for (int64_t t : ids) recompute_thread(tx, owner, t);
    res.threads_touched += static_cast<int64_t>(ids.size());
    tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));
  }
  return res;
}

}  // namespace azm::mail
