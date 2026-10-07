// Owner: WP-B (internal helpers; see send_internal.hpp)
#include "mail/send_internal.hpp"

#include "core/errors.hpp"
#include "core/strings.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "mail/fts.hpp"
#include "mail/html_text.hpp"
#include "mail/threads.hpp"
#include "ws/events.hpp"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>

namespace azm::mail::detail {
namespace json = boost::json;

namespace {

constexpr std::size_t kMaxReferences = 20;

json::array strings_json(const std::vector<std::string>& v) {
  json::array a;
  a.reserve(v.size());
  for (const auto& s : v) a.emplace_back(json::string(s));
  return a;
}

std::vector<std::string> strings_of(const json::object& o, std::string_view key) {
  std::vector<std::string> out;
  if (const auto* v = o.if_contains(key); v && v->is_array())
    for (const auto& el : v->as_array())
      if (el.is_string()) out.emplace_back(el.as_string());
  return out;
}

std::string string_of(const json::object& o, std::string_view key) {
  if (const auto* v = o.if_contains(key); v && v->is_string()) return std::string(v->as_string());
  return {};
}

std::optional<std::string> opt_string_of(const json::object& o, std::string_view key) {
  if (const auto* v = o.if_contains(key); v && v->is_string()) return std::string(v->as_string());
  return std::nullopt;
}

std::optional<int64_t> opt_int_of(const json::object& o, std::string_view key) {
  if (const auto* v = o.if_contains(key)) {
    if (v->is_int64()) return v->as_int64();
    if (v->is_uint64() && v->as_uint64() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return static_cast<int64_t>(v->as_uint64());
  }
  return std::nullopt;
}

}  // namespace

// ---- payload ----------------------------------------------------------------------------------

std::string payload_to_json(const FrozenPayload& p) {
  json::object o;
  o["v"] = 1;
  o["from"] = p.from;
  o["to"] = strings_json(p.to);
  o["cc"] = strings_json(p.cc);
  o["bcc"] = strings_json(p.bcc);
  o["reply_to"] = strings_json(p.reply_to);
  o["subject"] = p.subject;
  o["html"] = p.html;
  o["text"] = p.text;
  json::array ids;
  for (int64_t id : p.attachment_ids) ids.emplace_back(id);
  o["attachment_ids"] = std::move(ids);
  if (p.parent_message_id) o["parent_message_id"] = *p.parent_message_id;
  else o["parent_message_id"] = nullptr;
  if (p.in_reply_to) o["in_reply_to"] = *p.in_reply_to;
  else o["in_reply_to"] = nullptr;
  o["references"] = strings_json(p.references);
  json::object d;
  d["html"] = p.draft_html;
  if (p.draft_quoted_html) d["quoted_html"] = *p.draft_quoted_html;
  else d["quoted_html"] = nullptr;
  o["draft"] = std::move(d);
  return json::serialize(o);
}

FrozenPayload payload_from_json(std::string_view text) {
  FrozenPayload p;
  boost::system::error_code ec;
  const json::value v = json::parse(text, ec);
  if (ec || !v.is_object()) return p;
  const json::object& o = v.as_object();
  p.from = string_of(o, "from");
  p.to = strings_of(o, "to");
  p.cc = strings_of(o, "cc");
  p.bcc = strings_of(o, "bcc");
  p.reply_to = strings_of(o, "reply_to");
  p.subject = string_of(o, "subject");
  p.html = string_of(o, "html");
  p.text = string_of(o, "text");
  if (const auto* ids = o.if_contains("attachment_ids"); ids && ids->is_array())
    for (const auto& el : ids->as_array()) {
      if (el.is_int64()) p.attachment_ids.push_back(el.as_int64());
      else if (el.is_uint64()) p.attachment_ids.push_back(static_cast<int64_t>(el.as_uint64()));
    }
  p.parent_message_id = opt_int_of(o, "parent_message_id");
  p.in_reply_to = opt_string_of(o, "in_reply_to");
  p.references = strings_of(o, "references");
  if (const auto* d = o.if_contains("draft"); d && d->is_object()) {
    p.draft_html = string_of(d->as_object(), "html");
    p.draft_quoted_html = opt_string_of(d->as_object(), "quoted_html");
  }
  return p;
}

ResolvedParent resolve_parent_id(db::Conn& c, const FrozenPayload& p, int64_t sender_user_id,
                                 std::optional<int64_t> parent_outbound_id) {
  ResolvedParent r;
  r.id = p.in_reply_to;
  if (!r.id && p.parent_message_id)
    r.id = c.scalar<std::string>("SELECT message_id_header FROM messages WHERE id = ? AND owner_id = ?",
                                 *p.parent_message_id, sender_user_id);
  if (!r.id && parent_outbound_id) {
    auto s = c.prepare("SELECT message_id_header, resend_id FROM outbound WHERE id = ?");
    s.bind_all(*parent_outbound_id);
    if (s.step()) {
      r.id = s.opt_text(0);
      if (!r.id) {
        r.missing = true;
        r.parent_resend_id = s.opt_text(1);
      }
    }
  }
  if (r.id && r.id->empty()) r.id.reset();
  return r;
}

std::vector<std::string> sent_references(db::Conn& c, int64_t outbound_id) {
  auto s = c.prepare("SELECT payload_json, sender_user_id, parent_outbound_id FROM outbound WHERE id = ?");
  s.bind_all(outbound_id);
  if (!s.step()) return {};
  const FrozenPayload p = payload_from_json(s.text(0));
  const int64_t sender = s.i64(1);
  const std::optional<int64_t> parent_outbound = s.opt_i64(2);
  s.reset();
  return reference_chain(p.references, resolve_parent_id(c, p, sender, parent_outbound).id);
}

std::string format_msgid(std::string_view id) { return "<" + std::string(id) + ">"; }

std::string format_msgid_list(const std::vector<std::string>& ids) {
  std::string out;
  for (const auto& id : ids) {
    if (!out.empty()) out.push_back(' ');
    out += format_msgid(id);
  }
  return out;
}

std::vector<std::string> reference_chain(const std::vector<std::string>& base,
                                         const std::optional<std::string>& parent_id) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto add = [&](const std::string& raw) {
    std::string id = normalize_message_id(raw);
    if (id.empty() || !seen.insert(id).second) return;
    out.push_back(std::move(id));
  };
  for (const auto& r : base) add(r);
  if (parent_id) {
    // The parent id must end the chain even when it already appeared earlier.
    const std::string pid = normalize_message_id(*parent_id);
    if (!pid.empty()) {
      out.erase(std::remove(out.begin(), out.end(), pid), out.end());
      out.push_back(pid);
    }
  }
  if (out.size() > kMaxReferences) out.erase(out.begin(), out.end() - kMaxReferences);
  return out;
}

// ---- status -------------------------------------------------------------------------------------

StatusClass status_class(std::string_view st, std::optional<int64_t> scheduled_at) {
  const bool pending = st == "queued" || st == "sending";
  StatusClass c;
  c.sent = st != "scheduled" && st != "canceled" && !(pending && scheduled_at);
  c.scheduled = scheduled_at.has_value() && (pending || st == "accepted" || st == "scheduled");
  return c;
}

std::vector<OutboundCopy> outbound_copies(db::Conn& c, int64_t outbound_id) {
  std::vector<OutboundCopy> out;
  auto s = c.prepare(
      "SELECT owner_id, id, thread_id, is_shared_copy FROM messages WHERE outbound_id = ? AND "
      "is_draft = 0 ORDER BY is_shared_copy, id");
  s.bind_all(outbound_id);
  while (s.step()) out.push_back({s.i64(0), s.i64(1), s.i64(2), s.boolean(3)});
  return out;
}

bool record_delivery_event(db::Tx& tx, int64_t outbound_id, std::string_view type, int64_t occurred_at,
                           const boost::json::object& detail, std::string source_key) {
  const std::string detail_json = json::serialize(detail);
  if (!source_key.empty()) {
    tx.run(
        "INSERT OR IGNORE INTO delivery_events(outbound_id, type, occurred_at, detail_json, source_key) "
        "VALUES(?,?,?,?,?)",
        outbound_id, type, occurred_at, detail_json, source_key);
    return tx.changes() > 0;
  }
  int64_t n = tx.scalar<int64_t>("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ?", outbound_id)
                  .value_or(0);
  for (int attempt = 0; attempt < 16; ++attempt) {
    ++n;
    tx.run(
        "INSERT OR IGNORE INTO delivery_events(outbound_id, type, occurred_at, detail_json, source_key) "
        "VALUES(?,?,?,?,?)",
        outbound_id, type, occurred_at, detail_json, "local:" + std::to_string(n));
    if (tx.changes() > 0) return true;
  }
  return false;
}

void publish_outbound_change(db::Tx& tx, int64_t outbound_id, bool membership_changed) {
  auto s = tx.prepare("SELECT status, status_detail FROM outbound WHERE id = ?");
  s.bind_all(outbound_id);
  if (!s.step()) return;
  const std::string status = s.text(0);
  const std::optional<std::string> detail = s.opt_text(1);
  std::map<int64_t, std::vector<int64_t>> threads_by_owner;
  for (const auto& copy : outbound_copies(tx.conn(), outbound_id)) {
    recompute_thread(tx, copy.owner_id, copy.thread_id);
    threads_by_owner[copy.owner_id].push_back(copy.thread_id);
    std::optional<std::string_view> d;
    if (detail) d = *detail;
    tx.emit(copy.owner_id, std::string(ws::events::kOutboundStatus),
            ws::outbound_status_payload(copy.message_id, copy.thread_id, outbound_id, status, d));
  }
  if (membership_changed)
    for (auto& [owner, ids] : threads_by_owner) emit_threads_changed(tx, owner, std::move(ids));
}

void delete_shared_copies(db::Tx& tx, int64_t outbound_id) {
  struct Victim {
    int64_t owner, id, thread;
  };
  std::vector<Victim> victims;
  {
    auto s = tx.prepare(
        "SELECT owner_id, id, thread_id FROM messages WHERE outbound_id = ? AND is_shared_copy = 1 ORDER BY id");
    s.bind_all(outbound_id);
    while (s.step()) victims.push_back({s.i64(0), s.i64(1), s.i64(2)});
  }
  std::map<int64_t, std::vector<int64_t>> threads_by_owner;
  for (const auto& v : victims) {
    tx.run("DELETE FROM messages WHERE id = ? AND owner_id = ?", v.id, v.owner);
    threads_by_owner[v.owner].push_back(v.thread);
  }
  for (auto& [owner, ids] : threads_by_owner) {
    for (int64_t t : ids) recompute_thread(tx, owner, t);
    emit_threads_changed(tx, owner, std::move(ids));
  }
}

int64_t cancel_to_draft(db::Tx& tx, int64_t owner, int64_t outbound_id, int64_t now_ms) {
  auto o = tx.prepare("SELECT job_id, payload_json, status_detail FROM outbound WHERE id = ?");
  o.bind_all(outbound_id);
  if (!o.step()) throw ApiError::not_found("not_found", "邮件不存在");
  const std::optional<int64_t> job_id = o.opt_i64(0);
  const FrozenPayload payload = payload_from_json(o.text(1));
  o.reset();

  const auto sender_copy = [&]() -> std::optional<std::pair<int64_t, int64_t>> {
    auto s = tx.prepare(
        "SELECT id, thread_id FROM messages WHERE outbound_id = ? AND owner_id = ? AND is_shared_copy = 0 "
        "AND is_draft = 0 ORDER BY id LIMIT 1");
    s.bind_all(outbound_id, owner);
    if (!s.step()) return std::nullopt;
    return std::make_pair(s.i64(0), s.i64(1));
  }();
  if (!sender_copy) throw ApiError::not_found("not_found", "邮件不存在");
  const auto [message_id, thread_id] = *sender_copy;

  // A send job that is already running sees status 'canceled' in mark_sending and stops.
  if (job_id) jobs::cancel(tx, *job_id);
  record_delivery_event(tx, outbound_id, "local.canceled", now_ms, {});
  delete_shared_copies(tx, outbound_id);           // their owners get threads.changed
  publish_outbound_change(tx, outbound_id, false);  // outbound.status 'canceled' to the sender

  const std::string snippet = make_snippet(payload.draft_html, true);
  tx.run(
      "UPDATE messages SET is_draft = 1, direction = 'out', outbound_id = NULL, message_id_header = NULL, "
      "in_reply_to = NULL, draft_version = draft_version + 1, date = ?, updated_at = ?, in_inbox = 0, "
      "is_read = 1, is_spam = 0, trashed_at = NULL, snippet = ?, size_bytes = 0, "
      "has_attachments = EXISTS(SELECT 1 FROM attachments a WHERE a.message_id = messages.id AND a.is_inline = 0) "
      "WHERE id = ? AND owner_id = ?",
      now_ms, now_ms, snippet, message_id, owner);
  tx.run(
      "INSERT INTO message_bodies(message_id, html, text, quoted_html) VALUES(?, ?, NULL, ?) "
      "ON CONFLICT(message_id) DO UPDATE SET html = excluded.html, text = NULL, quoted_html = excluded.quoted_html",
      message_id, payload.draft_html, payload.draft_quoted_html);
  tx.run("DELETE FROM message_refs WHERE message_id = ?", message_id);
  recompute_thread(tx, owner, thread_id);
  fts_reindex(tx, message_id);
  emit_threads_changed(tx, owner, {thread_id});
  return message_id;
}

int64_t enqueue_send_job(db::Tx& tx, int64_t outbound_id, int64_t run_at, int64_t now_ms) {
  jobs::EnqueueOpts opts;
  opts.run_at_ms = run_at > 0 ? run_at : now_ms;
  opts.dedupe_key = jobs::dedupe_outbound_send(outbound_id);
  opts.max_attempts = jobs::kOutboundSendMaxAttempts;
  opts.priority = jobs::kPriorityHigh;
  opts.now_ms = now_ms;
  boost::json::object payload;
  payload[std::string(jobs::payload::kOutboundId)] = outbound_id;
  const int64_t job = jobs::enqueue(tx, jobs::kinds::kOutboundSend, std::move(payload), opts);
  tx.run("UPDATE outbound SET job_id = ? WHERE id = ?", job, outbound_id);
  return job;
}

// ---- misc ---------------------------------------------------------------------------------------

bool html_references_cid(std::string_view html, std::string_view content_id) {
  if (content_id.empty()) return false;
  const std::string h = to_lower_ascii(html);
  const std::string id = to_lower_ascii(content_id);
  return h.find("cid:" + id) != std::string::npos || h.find("cid:<" + id + ">") != std::string::npos ||
         h.find("cid:%3c" + id + "%3e") != std::string::npos;
}

bool is_local_domain(db::Conn& c, std::string_view email) {
  const std::string domain = to_lower_ascii(trim(domain_of(trim(email))));
  if (domain.empty()) return false;
  return c.scalar<int64_t>("SELECT 1 FROM domains WHERE name = ?", domain).has_value();
}

int64_t create_empty_thread(db::Tx& tx, int64_t owner, std::string_view subject, int64_t now_ms) {
  tx.run(
      "INSERT INTO threads(owner_id, subject, norm_subject, created_at, updated_at) VALUES(?,?,?,?,?)",
      owner, subject, normalize_subject(subject), now_ms, now_ms);
  return tx.last_insert_id();
}

void emit_threads_changed(db::Tx& tx, int64_t owner, std::vector<int64_t> ids) {
  std::vector<int64_t> uniq;
  std::set<int64_t> seen;
  for (int64_t id : ids)
    if (seen.insert(id).second) uniq.push_back(id);
  if (uniq.empty()) return;
  tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(uniq));
}

std::string clean_subject(std::string_view s) {
  std::string out = utf8_sanitize(s);
  for (char& c : out)
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  return std::string(trim(out));
}

}  // namespace azm::mail::detail
