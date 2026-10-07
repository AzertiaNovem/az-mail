// Owner: WP-B
// Drafts (C10), send-as (C3), queue_send freeze/validation (C4, C12, B1, B5) and undo-send.
#include "mail/drafts.hpp"

#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "mail/attachments.hpp"
#include "mail/fts.hpp"
#include "mail/html_scan.hpp"
#include "mail/html_text.hpp"
#include "mail/inbound.hpp"
#include "mail/internal.hpp"
#include "mail/mailbox.hpp"
#include "mail/render.hpp"
#include "mail/send_internal.hpp"
#include "mail/serde.hpp"
#include "mail/threads.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <charconv>
#include <set>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

namespace json = boost::json;
using detail::FrozenPayload;

constexpr int64_t kDayMs = 24LL * 3600 * 1000;

// Wrapper applied to every outgoing body so recipients see the same typography as the editor.
constexpr std::string_view kBodyStyle =
    "font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei','Noto Sans SC',"
    "'Helvetica Neue',Arial,sans-serif;font-size:14px;line-height:1.6;color:#202124";

// ---- errors -------------------------------------------------------------------------------------

ApiError draft_not_found() { return ApiError::not_found("not_found", "草稿不存在"); }
ApiError invalid_field(std::string_view field) {
  return ApiError::bad_request("invalid_field", "字段无效", {{"field", field}});
}
ApiError send_as_forbidden() { return ApiError::forbidden("send_as_forbidden", "无权使用该发件地址"); }
ApiError version_conflict(json::object details) {
  return ApiError::conflict("version_conflict", "草稿已在其他窗口修改", std::move(details));
}

// ---- rows ---------------------------------------------------------------------------------------

struct DraftRow {
  int64_t id = 0, thread_id = 0, version = 0, updated_at = 0;
  DraftMode mode = DraftMode::New;
  std::optional<int64_t> parent_message_id, from_address_id;
  std::vector<Address> to, cc, bcc;
  std::string subject, html;
  std::optional<std::string> quoted_html;
};

std::optional<DraftRow> load_draft_row(db::Conn& c, int64_t owner, int64_t id) {
  auto s = c.prepare(
      "SELECT m.id, m.thread_id, m.draft_version, m.updated_at, m.draft_mode, m.parent_message_id, "
      "m.from_address_id, m.to_json, m.cc_json, m.bcc_json, m.subject, b.html, b.quoted_html "
      "FROM messages m LEFT JOIN message_bodies b ON b.message_id = m.id "
      "WHERE m.id = ? AND m.owner_id = ? AND m.is_draft = 1");
  s.bind_all(id, owner);
  if (!s.step()) return std::nullopt;
  DraftRow r;
  r.id = s.i64(0);
  r.thread_id = s.i64(1);
  r.version = s.i64(2);
  r.updated_at = s.i64(3);
  if (auto m = s.opt_text(4)) r.mode = parse_draft_mode(*m).value_or(DraftMode::New);
  r.parent_message_id = s.opt_i64(5);
  r.from_address_id = s.opt_i64(6);
  r.to = detail::addresses_from_json(s.text(7));
  r.cc = detail::addresses_from_json(s.text(8));
  r.bcc = detail::addresses_from_json(s.text(9));
  r.subject = s.text(10);
  r.html = s.text(11);
  r.quoted_html = s.opt_text(12);
  return r;
}

// The message a reply / forward starts from (any non-draft message of the owner).
struct ParentInfo {
  int64_t id = 0, thread_id = 0;
  bool outgoing = false;
  std::optional<std::string> delivered_to;
  std::optional<int64_t> from_address_id, outbound_id, inbound_id;
  Address from;
  std::vector<Address> to, cc, reply_to;
  std::string subject;
  std::optional<std::string> message_id_header, in_reply_to, html;
};

std::optional<ParentInfo> load_parent(db::Conn& c, int64_t owner, int64_t id) {
  auto s = c.prepare(
      "SELECT m.id, m.thread_id, m.direction, m.delivered_to, m.from_address_id, m.outbound_id, "
      "m.inbound_id, m.from_name, m.from_email, m.to_json, m.cc_json, m.reply_to_json, m.subject, "
      "m.message_id_header, m.in_reply_to, b.html FROM messages m "
      "LEFT JOIN message_bodies b ON b.message_id = m.id "
      "WHERE m.id = ? AND m.owner_id = ? AND m.is_draft = 0");
  s.bind_all(id, owner);
  if (!s.step()) return std::nullopt;
  ParentInfo p;
  p.id = s.i64(0);
  p.thread_id = s.i64(1);
  p.outgoing = s.text(2) == "out";
  p.delivered_to = s.opt_text(3);
  p.from_address_id = s.opt_i64(4);
  p.outbound_id = s.opt_i64(5);
  p.inbound_id = s.opt_i64(6);
  p.from = Address{s.text(7), s.text(8)};
  p.to = detail::addresses_from_json(s.text(9));
  p.cc = detail::addresses_from_json(s.text(10));
  p.reply_to = detail::addresses_from_json(s.text(11));
  p.subject = s.text(12);
  p.message_id_header = s.opt_text(13);
  p.in_reply_to = s.opt_text(14);
  p.html = s.opt_text(15);
  return p;
}

// The parent's own References chain (ordered, without the parent's id), for the reply's
// References header: inbound parents keep it in inbound_emails.meta_json (message_refs is an
// unordered set), our own sent parents in their frozen payload.
std::vector<std::string> parent_references(db::Conn& c, const ParentInfo& p) {
  if (p.inbound_id) {
    if (auto meta = c.scalar<std::string>("SELECT meta_json FROM inbound_emails WHERE id = ?", *p.inbound_id)) {
      const json::object o = detail::object_from_json(*meta);
      if (const auto* v = o.if_contains("references"); v && v->is_array()) {
        std::vector<std::string> out;
        for (const auto& el : v->as_array())
          if (el.is_string()) out.emplace_back(el.as_string());
        if (out.empty() && p.in_reply_to) out.push_back(*p.in_reply_to);
        return out;
      }
    }
  }
  if (p.outbound_id && c.scalar<int64_t>("SELECT 1 FROM outbound WHERE id = ?", *p.outbound_id))
    return detail::sent_references(c, *p.outbound_id);
  std::vector<std::string> out;
  auto s = c.prepare("SELECT ref FROM message_refs WHERE message_id = ? ORDER BY ref");
  s.bind_all(p.id);
  while (s.step()) out.push_back(s.text(0));
  if (p.in_reply_to) {  // the direct parent goes last
    out.erase(std::remove(out.begin(), out.end(), *p.in_reply_to), out.end());
    out.push_back(*p.in_reply_to);
  }
  return out;
}

// ---- identities ---------------------------------------------------------------------------------

std::optional<SenderIdentity> try_sender(db::Conn& c, int64_t owner, int64_t address_id) {
  auto s = c.prepare(
      "SELECT a.id, a.email, a.kind, a.user_id, a.display_name, a.share_sent, u.display_name, "
      "(SELECT am.can_send_as FROM alias_members am WHERE am.alias_id = a.id AND am.user_id = ?) "
      "FROM addresses a LEFT JOIN users u ON u.id = a.user_id WHERE a.id = ?");
  s.bind_all(owner, address_id);
  if (!s.step()) return std::nullopt;
  SenderIdentity id;
  id.address_id = s.i64(0);
  id.address.email = s.text(1);
  const std::string kind = s.text(2);
  if (kind == "user") {
    if (s.opt_i64(3) != owner) return std::nullopt;
    id.address.name = s.text(4).empty() ? s.text(6) : s.text(4);
    id.is_alias = false;
    id.share_sent = false;
  } else {
    if (s.opt_i64(7).value_or(0) != 1) return std::nullopt;
    id.address.name = s.text(4);
    id.is_alias = true;
    id.share_sent = s.boolean(5);
  }
  return id;
}

// Default identity for a new draft: for replies the address the parent was delivered to (or
// sent from) when the owner may send as it, else the owner's own mailbox.
int64_t default_identity(db::Conn& c, int64_t owner, DraftMode mode, const std::optional<ParentInfo>& parent) {
  if (parent && (mode == DraftMode::Reply || mode == DraftMode::ReplyAll)) {
    std::vector<int64_t> candidates;
    if (parent->delivered_to)
      if (auto id = c.scalar<int64_t>("SELECT id FROM addresses WHERE email = ?", normalize_email(*parent->delivered_to)))
        candidates.push_back(*id);
    if (parent->outgoing && parent->from_address_id) candidates.push_back(*parent->from_address_id);
    for (int64_t cand : candidates)
      if (try_sender(c, owner, cand)) return cand;
  }
  return default_from_address(c, owner);
}

// ---- reply defaults -----------------------------------------------------------------------------

std::vector<Address> without(std::vector<Address> list, const std::set<std::string>& drop,
                             std::set<std::string>& seen) {
  std::vector<Address> out;
  for (auto& a : list) {
    const std::string key = normalize_email(a.email, true);
    if (key.empty() || drop.contains(key) || !seen.insert(key).second) continue;
    out.push_back(std::move(a));
  }
  return out;
}

bool has_forward_prefix(std::string_view subject) {
  const std::string_view s = trim(subject);
  for (std::string_view p : {"fwd:", "fw:", "fwd：", "fw：", "转发:", "转发：", "轉寄:", "轉寄："})
    if (istarts_with(s, p)) return true;
  return false;
}

// Server-side defaults for fields a reply/forward create request leaves absent (the frontend
// normally sends them; DraftInput absent = "use the default").
void fill_reply_defaults(db::Conn& c, int64_t owner, DraftMode mode, const ParentInfo& p, DraftInput& in) {
  if (!in.subject) {
    if (mode == DraftMode::Forward)
      in.subject = has_forward_prefix(p.subject) ? p.subject : "Fwd: " + p.subject;
    else
      in.subject = has_reply_prefix(p.subject) ? p.subject : "Re: " + p.subject;
  }
  if (mode == DraftMode::Forward) return;
  std::set<std::string> mine;
  for (const auto& e : detail::owner_addresses(c, owner)) mine.insert(normalize_email(e, true));
  std::set<std::string> seen;
  if (!in.to) {
    std::vector<Address> to;
    if (p.outgoing) {
      to = p.to;  // replying to our own sent mail goes to its recipients
    } else {
      to = p.reply_to.empty() ? std::vector<Address>{p.from} : p.reply_to;
      if (mode == DraftMode::ReplyAll) to.insert(to.end(), p.to.begin(), p.to.end());
    }
    std::vector<Address> kept = without(to, mine, seen);
    if (kept.empty() && !p.outgoing) kept = without({p.from}, {}, seen);  // mail to ourselves
    in.to = std::move(kept);
  }
  if (!in.cc && mode == DraftMode::ReplyAll) in.cc = without(p.cc, mine, seen);
}

// ---- HTML scanning ------------------------------------------------------------------------------

std::optional<int64_t> parse_positive(std::string_view s) {
  s = trim(s);
  if (s.empty() || s.size() > 18) return std::nullopt;
  int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v <= 0) return std::nullopt;
  return v;
}

void collect_file_ids(std::string_view value, std::set<int64_t>& out) {
  constexpr std::string_view kPath = "/api/files/";
  const std::string lower = to_lower_ascii(value);
  for (std::size_t p = lower.find(kPath); p != std::string::npos; p = lower.find(kPath, p + 1)) {
    std::size_t b = p + kPath.size(), e = b;
    while (e < value.size() && value[e] >= '0' && value[e] <= '9') ++e;
    if (e > b)
      if (auto id = parse_positive(value.substr(b, e - b))) out.insert(*id);
  }
}

// Attachment ids an HTML fragment points at: data-att-id values and /api/files/<id> URLs.
std::set<int64_t> referenced_attachment_ids(std::string_view h) {
  std::set<int64_t> out;
  std::size_t i = 0;
  while (i < h.size()) {
    const std::size_t lt = h.find('<', i);
    if (lt == std::string_view::npos) break;
    const auto m = html::parse_markup(h, lt);
    if (!m) {
      i = lt + 1;
      continue;
    }
    i = m->end;
    if (m->kind != html::MarkupKind::Tag || m->tag.closing) continue;
    for (const auto& a : m->tag.attrs) {
      if (!a.has_value) continue;
      if (a.name == "data-att-id") {
        if (auto id = parse_positive(html::decode_entities(a.raw_value(h)))) out.insert(*id);
      } else if (a.name == "src" || a.name == "href" || a.name == "srcset" || a.name == "background" ||
                 a.name == "poster" || a.name == "style") {
        collect_file_ids(html::decode_entities(a.raw_value(h)), out);
      }
    }
    if (html::is_raw_text_element(m->tag.name) && !m->tag.self_closing) {
      const std::size_t close = html::find_close_tag(h, i, m->tag.name);
      i = close == std::string_view::npos ? h.size() : close;
    }
  }
  return out;
}

// ---- attachments + body -------------------------------------------------------------------------

struct CurAtt {
  int64_t id = 0;
  std::optional<std::string> content_id;
  bool is_inline = false;
};

std::vector<CurAtt> draft_attachment_rows(db::Conn& c, int64_t owner, int64_t draft_id) {
  std::vector<CurAtt> out;
  auto s = c.prepare("SELECT id, content_id, is_inline FROM attachments WHERE message_id = ? AND owner_id = ? ORDER BY id");
  s.bind_all(draft_id, owner);
  while (s.step()) out.push_back({s.i64(0), s.opt_text(1), s.boolean(2)});
  return out;
}

std::vector<CidTarget> cid_targets(db::Conn& c, int64_t owner, int64_t message_id) {
  std::vector<CidTarget> out;
  auto s = c.prepare(
      "SELECT id, content_id FROM attachments WHERE message_id = ? AND owner_id = ? AND content_id IS NOT NULL "
      "AND content_id <> '' ORDER BY id");
  s.bind_all(message_id, owner);
  while (s.step()) out.push_back({s.i64(0), s.text(1)});
  return out;
}

void store_body(db::Tx& tx, int64_t message_id, std::string_view html, const std::optional<std::string>& quoted) {
  tx.run(
      "INSERT INTO message_bodies(message_id, html, text, quoted_html) VALUES(?, ?, NULL, ?) "
      "ON CONFLICT(message_id) DO UPDATE SET html = excluded.html, text = NULL, quoted_html = excluded.quoted_html",
      message_id, html, quoted);
}

// Applies attachment_ids / html / quoted_html of `in` to the draft (C10 stored-HTML invariant):
//  * inline uploads of the owner that the client HTML points at (data-att-id / file URL) are
//    attached even when attachment_ids omits them (E3 paste flow);
//  * attachment_ids is the full list: unlisted attachments are removed, except inline ones the
//    body / quote still references — by id, by cid:, or through the parent attachment the
//    quote's URL / data-att-id names (copied quote images the client may not list). On create
//    (`allow_removal` false) the list only adds uploads: the copied quote images are new to the
//    client;
//  * html and quoted_html are rewritten to cid: against the draft's attachments plus the
//    parent's (reply/forward quotes reference the parent's /api/files URLs, CONTRACTS §H 26).
void sync_content(db::Tx& tx, std::string_view base, int64_t owner, const DraftRow& row, const DraftInput& in,
                  bool allow_removal) {
  const std::string raw_html = in.html ? utf8_sanitize(*in.html) : row.html;
  std::optional<std::string> raw_quoted = row.quoted_html;
  if (in.quoted_html) {
    raw_quoted.reset();
    if (*in.quoted_html) raw_quoted = utf8_sanitize(**in.quoted_html);
  }

  std::set<int64_t> refs;
  if (in.html) refs = referenced_attachment_ids(raw_html);
  if (in.quoted_html && raw_quoted)
    for (int64_t id : referenced_attachment_ids(*raw_quoted)) refs.insert(id);

  const std::vector<CurAtt> current = draft_attachment_rows(tx.conn(), owner, row.id);
  auto on_draft = [&](int64_t id) {
    return std::any_of(current.begin(), current.end(), [&](const CurAtt& a) { return a.id == id; });
  };
  auto link = [&](int64_t id, bool inline_only) {
    tx.run(std::string("UPDATE attachments SET message_id = ? WHERE id = ? AND owner_id = ? AND message_id IS NULL") +
               (inline_only ? " AND is_inline = 1" : ""),
           row.id, id, owner);
    return tx.changes() > 0;
  };

  for (int64_t id : refs)
    if (!on_draft(id)) link(id, true);

  if (in.attachment_ids) {
    const std::set<int64_t> listed(in.attachment_ids->begin(), in.attachment_ids->end());
    for (int64_t id : listed)
      if (!on_draft(id)) link(id, false);
    for (int64_t id : listed)  // every listed id must now be on this draft
      if (!tx.scalar<int64_t>("SELECT 1 FROM attachments WHERE id = ? AND message_id = ? AND owner_id = ?", id,
                              row.id, owner))
        throw invalid_field("attachment_ids");
    if (allow_removal) {
      // Content-IDs the client HTML reaches through attachment ids (the draft's or the parent's).
      std::set<std::string> ref_cids;
      for (int64_t id : refs)
        if (auto cid = tx.scalar<std::string>("SELECT content_id FROM attachments WHERE id = ? AND owner_id = ?", id,
                                              owner);
            cid && !cid->empty())
          ref_cids.insert(to_lower_ascii(*cid));
      for (const auto& a : current) {
        if (listed.contains(a.id)) continue;
        const bool referenced =
            a.is_inline &&
            (refs.contains(a.id) ||
             (a.content_id && (ref_cids.contains(to_lower_ascii(*a.content_id)) ||
                               detail::html_references_cid(raw_html, *a.content_id) ||
                               (raw_quoted && detail::html_references_cid(*raw_quoted, *a.content_id)))));
        if (!referenced) tx.run("DELETE FROM attachments WHERE id = ? AND owner_id = ?", a.id, owner);
      }
    }
  }

  if (!in.html && !in.quoted_html) return;
  std::vector<CidTarget> targets = cid_targets(tx.conn(), owner, row.id);
  if (row.mode != DraftMode::New && row.parent_message_id) {
    const auto parent = cid_targets(tx.conn(), owner, *row.parent_message_id);
    targets.insert(targets.end(), parent.begin(), parent.end());
  }
  const std::string html = in.html ? rewrite_signed_to_cid(raw_html, targets, base) : row.html;
  std::optional<std::string> quoted = row.quoted_html;
  if (in.quoted_html) quoted = raw_quoted ? std::optional<std::string>(rewrite_signed_to_cid(*raw_quoted, targets, base))
                                          : std::nullopt;
  store_body(tx, row.id, html, quoted);
}

// Applies every present field of `in` (create defaults already filled in), bumps the version,
// refreshes snippet/date. mode, parent_message_id and include_parent_attachments are create-only.
void apply_draft_input(db::Tx& tx, std::string_view base, int64_t owner, const DraftRow& row, const DraftInput& in,
                       int64_t now, bool is_create = false) {
  if (in.from_address_id) {
    const SenderIdentity id = resolve_sender(tx.conn(), owner, *in.from_address_id);
    tx.run("UPDATE messages SET from_address_id = ?, from_name = ?, from_email = ? WHERE id = ? AND owner_id = ?",
           id.address_id, id.address.name, id.address.email, row.id, owner);
  }
  if (in.to)
    tx.run("UPDATE messages SET to_json = ? WHERE id = ? AND owner_id = ?", detail::addresses_to_json(*in.to), row.id,
           owner);
  if (in.cc)
    tx.run("UPDATE messages SET cc_json = ? WHERE id = ? AND owner_id = ?", detail::addresses_to_json(*in.cc), row.id,
           owner);
  if (in.bcc)
    tx.run("UPDATE messages SET bcc_json = ? WHERE id = ? AND owner_id = ?", detail::addresses_to_json(*in.bcc),
           row.id, owner);
  if (in.subject)
    tx.run("UPDATE messages SET subject = ? WHERE id = ? AND owner_id = ?", detail::clean_subject(*in.subject), row.id,
           owner);
  sync_content(tx, base, owner, row, in, !is_create);
  const std::string html =
      tx.scalar<std::string>("SELECT html FROM message_bodies WHERE message_id = ?", row.id).value_or("");
  tx.run(
      "UPDATE messages SET draft_version = draft_version + 1, snippet = ?, date = ?, updated_at = ?, "
      "has_attachments = EXISTS(SELECT 1 FROM attachments a WHERE a.message_id = messages.id AND a.is_inline = 0) "
      "WHERE id = ? AND owner_id = ?",
      make_snippet(html, true), now, now, row.id, owner);
}

void finish_draft_write(db::Tx& tx, int64_t owner, int64_t draft_id, int64_t thread_id) {
  recompute_thread(tx, owner, thread_id);
  fts_reindex(tx, draft_id);
  detail::emit_threads_changed(tx, owner, {thread_id});
}

// Copies the parent's quote images (attachments with a Content-ID that are inline or referenced
// by the parent's HTML) onto the draft as inline parts — or every attachment for a forward with
// include_parent_attachments — sharing the blob and keeping the Content-ID (CONTRACTS §H 26).
void copy_parent_attachments(db::Tx& tx, int64_t owner, int64_t draft_id, const ParentInfo& p, bool all,
                             int64_t now) {
  for (const auto& a : message_attachments(tx.conn(), owner, p.id)) {
    const bool has_cid = a.content_id && !a.content_id->empty();
    bool is_inline = a.is_inline;
    if (!all) {
      const bool referenced = has_cid && p.html && detail::html_references_cid(*p.html, *a.content_id);
      if (!has_cid || !(a.is_inline || referenced)) continue;
      is_inline = true;
    }
    tx.run(
        "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, content_id, "
        "is_inline, created_at) VALUES(?,?,?,?,?,?,?,?,?)",
        owner, draft_id, a.blob_sha256, a.filename, a.content_type, a.size, a.content_id, is_inline, now);
  }
}

// ---- send freeze --------------------------------------------------------------------------------

// The editor may already carry the signature (inserted client-side): don't append it twice.
// Image-only signatures have no text to compare and are always appended.
bool body_contains_signature(std::string_view body, std::string_view signature) {
  const std::string sig = detail::collapse_whitespace(html_to_text(signature));
  if (sig.empty()) return false;
  return detail::collapse_whitespace(html_to_text(body)).find(sig) != std::string::npos;
}

// body + signature (above the quote, unless the body already carries it) + quoted original,
// wrapped with inline styles; then data-att-id and our own file URLs are stripped (CONTRACTS
// §H 26) so only cid: references to local attachments leave the server.
std::string freeze_html(std::string_view body, std::string_view signature, const std::optional<std::string>& quoted,
                        std::string_view api_base) {
  std::string out = "<div style=\"" + std::string(kBodyStyle) + "\">";
  out += body;
  if (!trim(signature).empty() && !body_contains_signature(body, signature)) {
    out += "<div class=\"azm-signature\" style=\"margin-top:16px\">";
    out += signature;
    out += "</div>";
  }
  if (quoted && !trim(*quoted).empty()) {
    out += "<div class=\"gmail_quote azm-quote\" style=\"margin-top:16px\">";
    out += *quoted;
    out += "</div>";
  }
  out += "</div>";
  return strip_api_file_urls(strip_att_ids(out), api_base);
}

struct UserSendSettings {
  std::string signature_html;
  bool signature_enabled = true;
  int undo_send_seconds = 5;
};

UserSendSettings send_settings(db::Conn& c, int64_t owner) {
  UserSendSettings s;
  auto q = c.prepare("SELECT signature_html, signature_enabled, undo_send_seconds FROM user_settings WHERE user_id = ?");
  q.bind_all(owner);
  if (q.step()) {
    s.signature_html = q.text(0);
    s.signature_enabled = q.boolean(1);
    s.undo_send_seconds = static_cast<int>(std::clamp<int64_t>(q.i64(2), 0, 3600));
  }
  return s;
}

// Drops later duplicates (To before Cc before Bcc), case-insensitively ("a+x@" and "a@" are
// different recipients for the sender).
void dedupe_recipients(std::vector<Address>& to, std::vector<Address>& cc, std::vector<Address>& bcc) {
  std::set<std::string> seen;
  for (auto* list : {&to, &cc, &bcc}) {
    std::vector<Address> kept;
    for (auto& a : *list) {
      const std::string key = normalize_email(a.email);
      if (key.empty() || !seen.insert(key).second) continue;
      kept.push_back(std::move(a));
    }
    *list = std::move(kept);
  }
}

std::vector<std::string> formatted(const std::vector<Address>& list) {
  std::vector<std::string> out;
  out.reserve(list.size());
  for (const auto& a : list) out.push_back(format_address(a));
  return out;
}

std::string base_without_slash(std::string_view base) {
  base = trim(base);
  while (!base.empty() && base.back() == '/') base.remove_suffix(1);
  return std::string(base);
}

// Thread of the member's own copy of the conversation (same inbound / outbound as the parent),
// else regular threading on the references.
int64_t member_thread(db::Tx& tx, int64_t member, const std::optional<ParentInfo>& parent,
                      const ThreadingKeys& keys) {
  if (parent && parent->inbound_id)
    if (auto t = tx.scalar<int64_t>("SELECT thread_id FROM messages WHERE owner_id = ? AND inbound_id = ? LIMIT 1",
                                    member, *parent->inbound_id))
      return *t;
  if (parent && parent->outbound_id)
    if (auto t = tx.scalar<int64_t>(
            "SELECT thread_id FROM messages WHERE owner_id = ? AND outbound_id = ? AND is_draft = 0 LIMIT 1", member,
            *parent->outbound_id))
      return *t;
  return assign_thread(tx, member, keys);
}

struct FrozenMessage {
  int64_t sender_message_id = 0;
  int64_t outbound_id = 0;
  SenderIdentity ident;
  std::vector<Address> to, cc;
  std::string subject, snippet, html, text;
  std::optional<std::string> in_reply_to;
  std::vector<std::string> refs;
  bool has_attachments = false;
  int64_t size_bytes = 0;
};

// Alias send with share_sent=1 (C3): every other active member gets an outbound copy in their own
// thread (no BCC, C2), pointing at the same outbound row.
void create_shared_copies(db::Tx& tx, int64_t sender, const FrozenMessage& m, const std::optional<ParentInfo>& parent,
                          int64_t now) {
  std::vector<int64_t> members;
  {
    auto s = tx.prepare(
        "SELECT am.user_id FROM alias_members am JOIN users u ON u.id = am.user_id "
        "WHERE am.alias_id = ? AND am.user_id <> ? AND u.disabled = 0 ORDER BY am.user_id");
    s.bind_all(m.ident.address_id, sender);
    while (s.step()) members.push_back(s.i64(0));
  }
  if (members.empty()) return;
  const std::vector<AttachmentRecord> atts = message_attachments(tx.conn(), sender, m.sender_message_id);
  ThreadingKeys keys;
  keys.refs = m.refs;
  keys.subject = m.subject;
  keys.participants.push_back(normalize_email(m.ident.address.email));
  for (const auto* list : {&m.to, &m.cc})
    for (const auto& a : *list) keys.participants.push_back(normalize_email(a.email));
  keys.date = now;

  for (int64_t member : members) {
    const int64_t thread = member_thread(tx, member, parent, keys);
    tx.run(
        "INSERT INTO messages(owner_id, thread_id, direction, is_draft, outbound_id, is_shared_copy, sent_by_user_id, "
        "from_address_id, from_name, from_email, to_json, cc_json, bcc_json, reply_to_json, subject, snippet, date, "
        "in_reply_to, has_attachments, size_bytes, is_read, in_inbox, created_at, updated_at) "
        "VALUES(?, ?, 'out', 0, ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', ?, ?, ?, ?, ?, ?, 1, 0, ?, ?)",
        member, thread, m.outbound_id, sender, m.ident.address_id, m.ident.address.name, m.ident.address.email,
        detail::addresses_to_json(m.to), detail::addresses_to_json(m.cc), m.subject, m.snippet, now, m.in_reply_to,
        m.has_attachments, m.size_bytes, now, now);
    const int64_t copy = tx.last_insert_id();
    tx.run("INSERT INTO message_bodies(message_id, html, text, quoted_html) VALUES(?, ?, ?, NULL)", copy, m.html,
           m.text);
    for (const auto& r : m.refs)
      tx.run("INSERT OR IGNORE INTO message_refs(message_id, owner_id, ref) VALUES(?,?,?)", copy, member, r);
    for (const auto& a : atts)
      tx.run(
          "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, content_id, "
          "is_inline, created_at) VALUES(?,?,?,?,?,?,?,?,?)",
          member, copy, a.blob_sha256, a.filename, a.content_type, a.size, a.content_id, a.is_inline, now);
    recompute_thread(tx, member, thread);
    fts_reindex(tx, copy);
    detail::emit_threads_changed(tx, member, {thread});
  }
}

SendResult queue_send_impl(db::Tx& tx, const Config& cfg, const SignedUrls* urls, int64_t owner, int64_t draft_id,
                           const SendOptions& opts) {
  const int64_t now = opts.now_ms > 0 ? opts.now_ms : azm::now_ms();
  const std::string base = base_without_slash(urls ? urls->base_url() : cfg.public_api_base_url);
  auto row = load_draft_row(tx.conn(), owner, draft_id);
  if (!row) throw draft_not_found();
  if (opts.version != row->version) {
    json::object details;
    if (urls)
      if (auto cur = get_draft(tx.conn(), *urls, owner, draft_id)) details["current"] = to_json(*cur);
    throw version_conflict(std::move(details));
  }
  if (opts.draft) {
    apply_draft_input(tx, base, owner, *row, *opts.draft, now);
    row = load_draft_row(tx.conn(), owner, draft_id);
  }

  // 1. Validation (C3, C4, C12, B5).
  const int64_t from_id = row->from_address_id ? *row->from_address_id : default_from_address(tx.conn(), owner);
  const SenderIdentity ident = resolve_sender(tx.conn(), owner, from_id);

  dedupe_recipients(row->to, row->cc, row->bcc);
  if (row->to.empty() && row->cc.empty() && row->bcc.empty())
    throw ApiError::unprocessable("no_recipients", "请至少填写一个收件人");
  const std::size_t max_rcpt = static_cast<std::size_t>(std::max(1, cfg.max_recipients_per_field));
  for (const auto& [field, list] : {std::pair<std::string_view, const std::vector<Address>*>{"to", &row->to},
                                    {"cc", &row->cc},
                                    {"bcc", &row->bcc}})
    if (list->size() > max_rcpt)
      throw ApiError::unprocessable("too_many_recipients", "收件人过多（每栏最多 " + std::to_string(max_rcpt) + " 个）",
                                    {{"field", field}});
  {
    std::vector<std::string> emails;
    for (const auto* list : {&row->to, &row->cc, &row->bcc})
      for (const auto& a : *list) emails.push_back(a.email);
    const auto unknown = unknown_local_recipients(tx.conn(), emails);
    if (!unknown.empty()) {
      json::array arr;
      for (const auto& e : unknown) arr.emplace_back(json::string(e));
      throw ApiError::unprocessable("unknown_local_recipient", "收件人地址不存在", {{"emails", std::move(arr)}});
    }
  }
  if (opts.scheduled_at) {
    const int64_t lo = now + static_cast<int64_t>(cfg.schedule_min_lead_sec) * 1000;
    const int64_t hi = now + static_cast<int64_t>(cfg.schedule_max_days) * kDayMs;
    if (*opts.scheduled_at < lo || *opts.scheduled_at > hi)
      throw ApiError::unprocessable("invalid_schedule", "定时发送时间需在 1 分钟后至 " +
                                                            std::to_string(cfg.schedule_max_days) + " 天内");
  }

  // 2. Freeze the body.
  const UserSendSettings settings = send_settings(tx.conn(), owner);
  const std::string signature = settings.signature_enabled ? settings.signature_html : std::string();
  const std::string html = freeze_html(row->html, signature, row->quoted_html, cfg.public_api_base_url);
  const std::string text = html_to_text(html);

  // 3. Attachments: inline parts the frozen HTML no longer references are not sent (except for
  //    forwards, which keep everything); then the size limits (C12).
  std::vector<AttachmentRecord> keep, drop;
  for (auto& a : message_attachments(tx.conn(), owner, draft_id)) {
    const bool unreferenced =
        a.is_inline && !(a.content_id && detail::html_references_cid(html, *a.content_id));
    (row->mode != DraftMode::Forward && unreferenced ? drop : keep).push_back(std::move(a));
  }
  int64_t total = 0;
  for (const auto& a : keep) {
    if (a.size > static_cast<int64_t>(cfg.upload_body_limit))
      throw ApiError::too_large("message_too_large", "附件过大（单个附件不超过 25 MB）",
                                {{"attachment_id", a.id}, {"limit", static_cast<int64_t>(cfg.upload_body_limit)}});
    total += a.size;
  }
  if (total > static_cast<int64_t>(cfg.max_message_attachment_bytes))
    throw ApiError::too_large("message_too_large", "附件总大小超过限制",
                              {{"total_bytes", total}, {"limit", static_cast<int64_t>(cfg.max_message_attachment_bytes)}});
  for (const auto& a : drop) tx.run("DELETE FROM attachments WHERE id = ? AND owner_id = ?", a.id, owner);

  // 4. Threading headers (B2): parent's References + parent id; resolved again at send time.
  std::optional<ParentInfo> parent;
  if (row->mode != DraftMode::New && row->parent_message_id)
    parent = load_parent(tx.conn(), owner, *row->parent_message_id);
  std::optional<std::string> in_reply_to;
  std::vector<std::string> refs_base;
  std::optional<int64_t> parent_outbound_id;
  if (parent) {
    if (parent->message_id_header && !parent->message_id_header->empty()) in_reply_to = parent->message_id_header;
    refs_base = parent_references(tx.conn(), *parent);
    if (parent->outgoing) parent_outbound_id = parent->outbound_id;
  }
  const std::vector<std::string> refs = detail::reference_chain(refs_base, in_reply_to);

  // 5. The outbound row (new uuid per send = new Idempotency-Key, B1).
  std::optional<ScheduledVia> via;
  if (opts.scheduled_at)
    via = (!keep.empty() || cfg.schedule_mode == ScheduleMode::Local) ? ScheduledVia::Local : ScheduledVia::Resend;
  const int undo_s = opts.scheduled_at ? 0 : settings.undo_send_seconds;
  const int64_t send_after = now + static_cast<int64_t>(undo_s) * 1000;

  FrozenPayload p;
  p.from = format_address(ident.address);
  p.to = formatted(row->to);
  p.cc = formatted(row->cc);
  p.bcc = formatted(row->bcc);
  p.subject = row->subject;
  p.html = html;
  p.text = text;
  for (const auto& a : keep) p.attachment_ids.push_back(a.id);
  if (parent) p.parent_message_id = parent->id;
  p.in_reply_to = in_reply_to;
  p.references = refs_base;
  p.draft_html = row->html;
  p.draft_quoted_html = row->quoted_html;

  const std::string uuid = crypto::uuid_v4();
  tx.run(
      "INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, scheduled_at, scheduled_via, "
      "parent_outbound_id, payload_json, total_bytes, created_at, updated_at) "
      "VALUES(?, ?, ?, 'queued', ?, ?, ?, ?, ?, ?, ?, ?)",
      uuid, owner, ident.address_id, send_after, opts.scheduled_at,
      via ? std::optional<std::string>(std::string(to_string(*via))) : std::nullopt, parent_outbound_id,
      detail::payload_to_json(p), total, now, now);
  const int64_t outbound_id = tx.last_insert_id();

  // 6. The draft becomes the sender's sent copy.
  FrozenMessage fm;
  fm.sender_message_id = draft_id;
  fm.outbound_id = outbound_id;
  fm.ident = ident;
  fm.to = row->to;
  fm.cc = row->cc;
  fm.subject = row->subject;
  fm.snippet = make_snippet(html, true);
  fm.html = html;
  fm.text = text;
  fm.in_reply_to = in_reply_to;
  fm.refs = refs;
  fm.has_attachments = std::any_of(keep.begin(), keep.end(), [](const AttachmentRecord& a) { return !a.is_inline; });
  fm.size_bytes = static_cast<int64_t>(html.size() + text.size()) + total;

  tx.run(
      "UPDATE messages SET is_draft = 0, direction = 'out', outbound_id = ?, from_address_id = ?, from_name = ?, "
      "from_email = ?, to_json = ?, cc_json = ?, bcc_json = ?, reply_to_json = '[]', subject = ?, snippet = ?, "
      "date = ?, message_id_header = NULL, in_reply_to = ?, has_attachments = ?, size_bytes = ?, is_read = 1, "
      "in_inbox = 0, is_spam = 0, trashed_at = NULL, updated_at = ? WHERE id = ? AND owner_id = ?",
      outbound_id, ident.address_id, ident.address.name, ident.address.email, detail::addresses_to_json(row->to),
      detail::addresses_to_json(row->cc), detail::addresses_to_json(row->bcc), row->subject, fm.snippet, now,
      in_reply_to, fm.has_attachments, fm.size_bytes, now, draft_id, owner);
  tx.run(
      "INSERT INTO message_bodies(message_id, html, text, quoted_html) VALUES(?, ?, ?, NULL) "
      "ON CONFLICT(message_id) DO UPDATE SET html = excluded.html, text = excluded.text, quoted_html = NULL",
      draft_id, html, text);
  tx.run("DELETE FROM message_refs WHERE message_id = ?", draft_id);
  for (const auto& r : refs)
    tx.run("INSERT OR IGNORE INTO message_refs(message_id, owner_id, ref) VALUES(?,?,?)", draft_id, owner, r);

  if (ident.is_alias && ident.share_sent) create_shared_copies(tx, owner, fm, parent, now);

  recompute_thread(tx, owner, row->thread_id);
  fts_reindex(tx, draft_id);
  for (const auto* list : {&row->to, &row->cc, &row->bcc})
    for (const auto& a : *list) upsert_contact(tx, owner, a, 1.0, now);

  json::object ev;
  if (opts.scheduled_at) {
    ev["scheduled_at"] = *opts.scheduled_at;
    ev["scheduled_via"] = to_string(*via);
  }
  detail::record_delivery_event(tx, outbound_id, "local.queued", now, ev);

  // 7. The send job: after the undo window, or at the scheduled time for local scheduling.
  const int64_t run_at = via == ScheduledVia::Local ? *opts.scheduled_at : send_after;
  detail::enqueue_send_job(tx, outbound_id, run_at, now);
  detail::emit_threads_changed(tx, owner, {row->thread_id});

  SendResult r;
  r.message_id = draft_id;
  r.thread_id = row->thread_id;
  r.outbound_id = outbound_id;
  r.status = OutboundStatus::Queued;
  r.undo_ms = static_cast<int64_t>(undo_s) * 1000;
  r.scheduled_at = opts.scheduled_at;
  return r;
}

}  // namespace

// =============================================================================================
// Identities
// =============================================================================================

SenderIdentity resolve_sender(db::Conn& c, int64_t owner, int64_t address_id) {
  auto id = try_sender(c, owner, address_id);
  if (!id) throw send_as_forbidden();
  return *id;
}

int64_t default_from_address(db::Conn& c, int64_t owner) {
  auto id = c.scalar<int64_t>("SELECT id FROM addresses WHERE kind = 'user' AND user_id = ? ORDER BY id LIMIT 1", owner);
  if (!id) throw ApiError::not_found("not_found", "用户没有邮箱地址");
  return *id;
}

// =============================================================================================
// Drafts
// =============================================================================================

std::optional<Draft> get_draft(db::Conn& c, const SignedUrls& urls, int64_t owner, int64_t draft_id) {
  auto row = load_draft_row(c, owner, draft_id);
  if (!row) return std::nullopt;
  Draft d;
  d.id = row->id;
  d.thread_id = row->thread_id;
  d.version = row->version;
  d.mode = row->mode;
  d.parent_message_id = row->parent_message_id;
  d.from_address_id = row->from_address_id ? *row->from_address_id : default_from_address(c, owner);
  d.to = std::move(row->to);
  d.cc = std::move(row->cc);
  d.bcc = std::move(row->bcc);
  d.subject = std::move(row->subject);
  d.updated_at = row->updated_at;
  const int64_t exp = urls.expiry(azm::now_ms());
  std::vector<CidTarget> targets;
  for (const auto& a : message_attachments(c, owner, draft_id)) {
    if (a.content_id && !a.content_id->empty()) targets.push_back({a.id, *a.content_id});
    d.attachments.push_back(make_attachment_view(a, urls, owner, exp));
  }
  d.html = rewrite_cid_to_signed(row->html, targets, urls, owner, exp);
  if (row->quoted_html) d.quoted_html = rewrite_cid_to_signed(*row->quoted_html, targets, urls, owner, exp);
  return d;
}

Draft create_draft(db::Tx& tx, const SignedUrls& urls, int64_t owner, const DraftInput& input) {
  const int64_t now = azm::now_ms();
  DraftInput in = input;
  const DraftMode mode = in.mode.value_or(DraftMode::New);
  std::optional<ParentInfo> parent;
  if (mode != DraftMode::New) {
    const std::optional<int64_t> pid = in.parent_message_id ? *in.parent_message_id : std::nullopt;
    if (!pid) throw invalid_field("parent_message_id");
    parent = load_parent(tx.conn(), owner, *pid);
    if (!parent) throw ApiError::not_found("not_found", "原邮件不存在");
  }
  const int64_t from_id = in.from_address_id ? resolve_sender(tx.conn(), owner, *in.from_address_id).address_id
                                             : default_identity(tx.conn(), owner, mode, parent);
  in.from_address_id = from_id;
  if (parent) fill_reply_defaults(tx.conn(), owner, mode, *parent, in);
  const std::string subject = detail::clean_subject(in.subject.value_or(""));
  in.subject = subject;

  const int64_t thread = parent ? parent->thread_id : detail::create_empty_thread(tx, owner, subject, now);
  tx.run(
      "INSERT INTO messages(owner_id, thread_id, direction, is_draft, subject, date, is_read, in_inbox, draft_version, "
      "draft_mode, parent_message_id, created_at, updated_at) VALUES(?, ?, 'out', 1, ?, ?, 1, 0, 0, ?, ?, ?, ?)",
      owner, thread, subject, now, to_string(mode), parent ? std::optional<int64_t>(parent->id) : std::nullopt, now,
      now);
  const int64_t draft_id = tx.last_insert_id();
  store_body(tx, draft_id, "", std::nullopt);
  if (parent)
    copy_parent_attachments(tx, owner, draft_id, *parent,
                            mode == DraftMode::Forward && in.include_parent_attachments.value_or(false), now);
  if (!in.html) in.html = std::string();
  const DraftRow row = *load_draft_row(tx.conn(), owner, draft_id);
  apply_draft_input(tx, urls.base_url(), owner, row, in, now, /*is_create=*/true);
  finish_draft_write(tx, owner, draft_id, thread);
  return *get_draft(tx.conn(), urls, owner, draft_id);
}

Draft update_draft(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t id, int64_t version,
                   const DraftInput& in, bool force) {
  const auto row = load_draft_row(tx.conn(), owner, id);
  if (!row) throw draft_not_found();
  if (!force && version != row->version) {
    json::object details;
    details["current"] = to_json(*get_draft(tx.conn(), urls, owner, id));
    throw version_conflict(std::move(details));
  }
  apply_draft_input(tx, urls.base_url(), owner, *row, in, azm::now_ms());
  finish_draft_write(tx, owner, id, row->thread_id);
  return *get_draft(tx.conn(), urls, owner, id);
}

void delete_draft(db::Tx& tx, int64_t owner, int64_t id) {
  const auto row = load_draft_row(tx.conn(), owner, id);
  if (!row) throw draft_not_found();
  tx.run("DELETE FROM messages WHERE id = ? AND owner_id = ? AND is_draft = 1", id, owner);
  recompute_thread(tx, owner, row->thread_id);
  detail::emit_threads_changed(tx, owner, {row->thread_id});
}

// =============================================================================================
// Send
// =============================================================================================

SendResult queue_send(db::Tx& tx, const Config& cfg, const SignedUrls& urls, int64_t owner, int64_t draft_id,
                      const SendOptions& opts) {
  return queue_send_impl(tx, cfg, &urls, owner, draft_id, opts);
}

SendResult queue_send(db::Tx& tx, const Config& cfg, int64_t owner, int64_t draft_id, const SendOptions& opts) {
  return queue_send_impl(tx, cfg, nullptr, owner, draft_id, opts);
}

Draft undo_send(db::Tx& tx, const SignedUrls& urls, int64_t owner, int64_t message_id) {
  const int64_t now = azm::now_ms();
  auto s = tx.prepare(
      "SELECT m.is_draft, m.is_shared_copy, m.outbound_id, o.sender_user_id FROM messages m "
      "LEFT JOIN outbound o ON o.id = m.outbound_id WHERE m.id = ? AND m.owner_id = ?");
  s.bind_all(message_id, owner);
  if (!s.step()) throw ApiError::not_found("not_found", "邮件不存在");
  const bool is_draft = s.boolean(0);
  const bool shared = s.boolean(1);
  const std::optional<int64_t> outbound_id = s.opt_i64(2);
  const std::optional<int64_t> sender = s.opt_i64(3);
  s.reset();
  if (is_draft) throw ApiError::conflict("too_late", "邮件已撤回或无法撤销");
  if (shared || !outbound_id || sender != owner) throw ApiError::not_found("not_found", "邮件不存在");

  tx.run(
      "UPDATE outbound SET status = 'canceled', status_detail = NULL, updated_at = ? "
      "WHERE id = ? AND status = 'queued' AND scheduled_at IS NULL",
      now, *outbound_id);
  if (tx.changes() == 0) throw ApiError::conflict("too_late", "邮件已发出，无法撤销");
  const int64_t draft_id = detail::cancel_to_draft(tx, owner, *outbound_id, now);
  return *get_draft(tx.conn(), urls, owner, draft_id);
}

}  // namespace azm::mail
