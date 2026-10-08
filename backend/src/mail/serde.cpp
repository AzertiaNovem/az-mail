// Owner: WP-B
// JSON <-> mail structs (serde.hpp). Shapes follow docs/API.md + Addendum B exactly: every
// key is always present, optionals serialize as null, times are ms numbers.
#include "mail/serde.hpp"

#include "core/address.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"

#include <boost/json/string.hpp>

#include <string>

namespace azm::mail {
namespace {

namespace json = boost::json;

constexpr std::size_t kMaxAddresses = 1000;   // sanity bound; per-field limits are checked at send
constexpr std::size_t kMaxThreadIds = 1000;

json::value opt(const std::optional<std::string>& v) {
  if (!v) return nullptr;
  return json::value(json::string(*v));
}
json::value opt(const std::optional<int64_t>& v) {
  if (!v) return nullptr;
  return json::value(*v);
}

json::array ids_json(std::span<const int64_t> ids) {
  json::array a;
  a.reserve(ids.size());
  for (int64_t id : ids) a.emplace_back(id);
  return a;
}

template <class T>
json::array list_json(const std::vector<T>& items) {
  json::array a;
  a.reserve(items.size());
  for (const auto& it : items) a.emplace_back(to_json(it));
  return a;
}

std::string field_name(std::string_view prefix, std::string_view key) {
  return std::string(prefix) + std::string(key);
}

// Absent and null are both "absent" (core/json convention), except where noted.
const json::value* present(const json::object& o, std::string_view key) {
  const json::value* v = o.if_contains(key);
  if (v == nullptr || v->is_null()) return nullptr;
  return v;
}

std::optional<std::string> get_string(const json::object& o, std::string_view key, std::string_view prefix) {
  const json::value* v = present(o, key);
  if (v == nullptr) return std::nullopt;
  if (!v->is_string()) throw_invalid_field(field_name(prefix, key));
  return std::string(v->as_string());
}

std::optional<int64_t> get_int(const json::object& o, std::string_view key, std::string_view prefix) {
  const json::value* v = present(o, key);
  if (v == nullptr) return std::nullopt;
  auto i = as_int64(*v);
  if (!i) throw_invalid_field(field_name(prefix, key));
  return i;
}

std::optional<bool> get_bool(const json::object& o, std::string_view key, std::string_view prefix) {
  const json::value* v = present(o, key);
  if (v == nullptr) return std::nullopt;
  if (!v->is_bool()) throw_invalid_field(field_name(prefix, key));
  return v->as_bool();
}

std::optional<std::vector<int64_t>> get_ids(const json::object& o, std::string_view key,
                                            std::string_view prefix, std::size_t max) {
  const json::value* v = present(o, key);
  if (v == nullptr) return std::nullopt;
  const std::string name = field_name(prefix, key);
  if (!v->is_array() || v->as_array().size() > max) throw_invalid_field(name);
  std::vector<int64_t> out;
  out.reserve(v->as_array().size());
  for (const auto& el : v->as_array()) {
    auto i = as_int64(el);
    if (!i) throw_invalid_field(name);
    out.push_back(*i);
  }
  return out;
}

std::optional<std::vector<Address>> get_addresses(const json::object& o, std::string_view key,
                                                  std::string_view prefix) {
  const json::value* v = present(o, key);
  if (v == nullptr) return std::nullopt;
  return parse_address_list(*v, field_name(prefix, key));
}

}  // namespace

// ---- output ----------------------------------------------------------------------------------

json::object to_json(const Address& a) {
  json::object o;
  o["name"] = a.name;
  o["email"] = a.email;
  return o;
}

json::array to_json(std::span<const Address> list) {
  json::array a;
  a.reserve(list.size());
  for (const auto& x : list) a.emplace_back(to_json(x));
  return a;
}

json::object to_json(const AttachmentView& a) {
  json::object o;
  o["id"] = a.id;
  o["filename"] = a.filename;
  o["content_type"] = a.content_type;
  o["size"] = a.size;
  o["inline"] = a.is_inline;
  o["content_id"] = opt(a.content_id);
  o["download_url"] = a.download_url;
  o["view_url"] = opt(a.view_url);
  return o;
}

json::object to_json(const OutboundView& v) {
  json::object o;
  o["id"] = v.id;
  o["status"] = to_string(v.status);
  o["status_detail"] = opt(v.status_detail);
  o["scheduled_at"] = opt(v.scheduled_at);
  if (v.scheduled_via) o["scheduled_via"] = to_string(*v.scheduled_via);
  else o["scheduled_via"] = nullptr;
  o["undo_until"] = opt(v.undo_until);
  o["sent_at"] = opt(v.sent_at);
  return o;
}

json::object to_json(const AuthResults& a) {
  json::object o;
  o["spf"] = opt(a.spf);
  o["dkim"] = opt(a.dkim);
  o["dmarc"] = opt(a.dmarc);
  return o;
}

json::object to_json(const MessageView& m) {
  json::object o;
  o["id"] = m.id;
  o["thread_id"] = m.thread_id;
  o["direction"] = to_string(m.direction);
  o["is_draft"] = m.is_draft;
  o["from"] = to_json(m.from);
  if (m.sent_by) o["sent_by"] = to_json(*m.sent_by);
  else o["sent_by"] = nullptr;
  o["to"] = to_json(std::span<const Address>(m.to));
  o["cc"] = to_json(std::span<const Address>(m.cc));
  o["bcc"] = to_json(std::span<const Address>(m.bcc));
  o["reply_to"] = to_json(std::span<const Address>(m.reply_to));
  o["delivered_to"] = opt(m.delivered_to);
  o["subject"] = m.subject;
  o["snippet"] = m.snippet;
  o["date"] = m.date;
  o["html"] = opt(m.html);
  o["text"] = opt(m.text);
  o["attachments"] = list_json(m.attachments);
  o["is_read"] = m.is_read;
  o["is_starred"] = m.is_starred;
  o["in_inbox"] = m.in_inbox;
  o["is_spam"] = m.is_spam;
  o["trashed"] = m.trashed;
  o["label_ids"] = ids_json(m.label_ids);
  if (m.auth) o["auth"] = to_json(*m.auth);
  else o["auth"] = nullptr;
  json::array w;
  for (const auto& s : m.warnings) w.emplace_back(json::string(s));
  o["warnings"] = std::move(w);
  if (m.outbound) o["outbound"] = to_json(*m.outbound);
  else o["outbound"] = nullptr;
  o["message_id_header"] = opt(m.message_id_header);
  o["raw_url"] = opt(m.raw_url);
  return o;
}

json::object to_json(const Participant& p) {
  json::object o;
  o["name"] = p.name;
  o["email"] = p.email;
  o["is_me"] = p.is_me;
  o["unread"] = p.unread;
  return o;
}

json::object to_json(const ThreadListItem& t) {
  json::object o;
  o["id"] = t.id;
  o["subject"] = t.subject;
  o["snippet"] = t.snippet;
  o["participants"] = list_json(t.participants);
  o["message_count"] = t.message_count;
  o["draft_count"] = t.draft_count;
  o["unread"] = t.unread;
  o["starred"] = t.starred;
  o["has_attachments"] = t.has_attachments;
  o["label_ids"] = ids_json(t.label_ids);
  o["last_at"] = t.last_at;
  o["in_inbox"] = t.in_inbox;
  if (t.latest_status) o["latest_status"] = to_string(*t.latest_status);
  else o["latest_status"] = nullptr;
  o["scheduled_at"] = opt(t.scheduled_at);
  json::array prev;
  for (const auto& a : t.attachments_preview) {
    json::object x;
    x["id"] = a.id;
    x["filename"] = a.filename;
    x["content_type"] = a.content_type;
    prev.emplace_back(std::move(x));
  }
  o["attachments_preview"] = std::move(prev);
  if (!t.to_preview.empty()) {  // additive, optional (F16): omitted when the view has no sent mail
    json::array to_prev;
    for (const auto& p : t.to_preview) {
      json::object x;
      x["name"] = p.name;
      x["email"] = p.email;
      x["is_me"] = p.is_me;
      to_prev.emplace_back(std::move(x));
    }
    o["to_preview"] = std::move(to_prev);
  }
  return o;
}

json::object to_json(const ThreadPage& p) {
  json::object o;
  o["items"] = list_json(p.items);
  o["next_cursor"] = opt(p.next_cursor);
  o["total"] = opt(p.total);
  return o;
}

json::object to_json(const ThreadDetail& t) {
  json::object o;
  o["id"] = t.id;
  o["subject"] = t.subject;
  o["label_ids"] = ids_json(t.label_ids);
  o["messages"] = list_json(t.messages);
  return o;
}

json::object to_json(const Draft& d) {
  json::object o;
  o["id"] = d.id;
  o["thread_id"] = d.thread_id;
  o["version"] = d.version;
  o["mode"] = to_string(d.mode);
  o["parent_message_id"] = opt(d.parent_message_id);
  o["from_address_id"] = d.from_address_id;
  o["to"] = to_json(std::span<const Address>(d.to));
  o["cc"] = to_json(std::span<const Address>(d.cc));
  o["bcc"] = to_json(std::span<const Address>(d.bcc));
  o["subject"] = d.subject;
  o["html"] = d.html;
  o["quoted_html"] = opt(d.quoted_html);
  o["attachments"] = list_json(d.attachments);
  o["updated_at"] = d.updated_at;
  return o;
}

json::object to_json(const SendResult& r) {
  json::object o;
  o["message_id"] = r.message_id;
  o["thread_id"] = r.thread_id;
  o["outbound_id"] = r.outbound_id;
  o["status"] = to_string(r.status);
  o["undo_ms"] = r.undo_ms;
  o["scheduled_at"] = opt(r.scheduled_at);
  return o;
}

json::object to_json(const Counts& c) {
  json::object o;
  o["inbox_unread"] = c.inbox_unread;
  o["drafts"] = c.drafts;
  o["scheduled"] = c.scheduled;
  o["spam_unread"] = c.spam_unread;
  json::object labels;
  for (const auto& [id, lc] : c.labels) {
    json::object x;
    x["unread"] = lc.unread;
    x["total"] = lc.total;
    labels[std::to_string(id)] = std::move(x);
  }
  o["labels"] = std::move(labels);
  return o;
}

json::object to_json(const ContactView& c) {
  json::object o;
  o["name"] = c.name;
  o["email"] = c.email;
  o["kind"] = to_string(c.kind);
  return o;
}

json::object to_json(const DeliveryEventView& e) {
  json::object o;
  o["type"] = e.type;
  o["occurred_at"] = e.occurred_at;
  o["detail"] = e.detail;
  return o;
}

json::object contacts_response(std::span<const ContactView> items) {
  json::array a;
  a.reserve(items.size());
  for (const auto& c : items) a.emplace_back(to_json(c));
  json::object o;
  o["items"] = std::move(a);
  return o;
}

json::object events_response(std::span<const DeliveryEventView> events) {
  json::array a;
  a.reserve(events.size());
  for (const auto& e : events) a.emplace_back(to_json(e));
  json::object o;
  o["events"] = std::move(a);
  return o;
}

json::object draft_response(const Draft& d) {
  json::object o;
  o["draft"] = to_json(d);
  return o;
}

json::object thread_ids_response(std::span<const int64_t> ids) {
  json::object o;
  o["thread_ids"] = ids_json(ids);
  return o;
}

// ---- input -----------------------------------------------------------------------------------

Address parse_address(const json::value& v, std::string_view field) {
  const json::object* o = v.if_object();
  if (o == nullptr) throw_invalid_field(field);
  Address a;
  const json::value* email = o->if_contains("email");
  if (email == nullptr || !email->is_string()) throw_invalid_field(field);
  a.email = std::string(trim(std::string_view(email->as_string())));
  if (!is_valid_email(a.email)) throw_invalid_field(field);
  if (const json::value* name = o->if_contains("name"); name != nullptr && !name->is_null()) {
    if (!name->is_string()) throw_invalid_field(field);
    a.name = std::string(trim(std::string_view(name->as_string())));
    if (!utf8_valid(a.name)) throw_invalid_field(field);
  }
  return a;
}

std::vector<Address> parse_address_list(const json::value& v, std::string_view field) {
  const json::array* arr = v.if_array();
  if (arr == nullptr || arr->size() > kMaxAddresses) throw_invalid_field(field);
  std::vector<Address> out;
  out.reserve(arr->size());
  for (const auto& el : *arr) out.push_back(parse_address(el, field));
  return out;
}

DraftInput parse_draft_input(const json::object& o, std::string_view prefix) {
  DraftInput in;
  if (auto m = get_string(o, "mode", prefix)) {
    in.mode = parse_draft_mode(*m);
    if (!in.mode) throw_invalid_field(field_name(prefix, "mode"));
  }
  // parent_message_id: present-null and absent are distinct (nested optional).
  if (const json::value* v = o.if_contains("parent_message_id")) {
    if (v->is_null()) {
      in.parent_message_id.emplace(std::nullopt);
    } else {
      auto id = as_int64(*v);
      if (!id) throw_invalid_field(field_name(prefix, "parent_message_id"));
      in.parent_message_id.emplace(*id);
    }
  }
  in.from_address_id = get_int(o, "from_address_id", prefix);
  in.to = get_addresses(o, "to", prefix);
  in.cc = get_addresses(o, "cc", prefix);
  in.bcc = get_addresses(o, "bcc", prefix);
  in.subject = get_string(o, "subject", prefix);
  in.html = get_string(o, "html", prefix);
  if (const json::value* v = o.if_contains("quoted_html")) {
    if (v->is_null()) in.quoted_html.emplace(std::nullopt);
    else if (v->is_string()) in.quoted_html.emplace(std::string(v->as_string()));
    else throw_invalid_field(field_name(prefix, "quoted_html"));
  }
  in.attachment_ids = get_ids(o, "attachment_ids", prefix, kMaxAddresses);
  in.include_parent_attachments = get_bool(o, "include_parent_attachments", prefix);
  return in;
}

DraftUpdate parse_draft_update(const json::object& o) {
  DraftUpdate u;
  const auto version = get_int(o, "version", "");
  if (!version) throw_invalid_field("version");
  u.version = *version;
  u.force = get_bool(o, "force", "").value_or(false);
  u.input = parse_draft_input(o);
  return u;
}

SendOptions parse_send_options(const json::object& o) {
  SendOptions s;
  const auto version = get_int(o, "version", "");
  if (!version) throw_invalid_field("version");
  s.version = *version;
  if (const json::value* d = present(o, "draft")) {
    if (!d->is_object()) throw_invalid_field("draft");
    s.draft = parse_draft_input(d->as_object(), "draft.");
  }
  s.scheduled_at = get_int(o, "scheduled_at", "");
  return s;
}

MessagePatch parse_message_patch(const json::object& o) {
  MessagePatch p;
  p.is_read = get_bool(o, "is_read", "");
  p.is_starred = get_bool(o, "is_starred", "");
  p.add_label_ids = get_ids(o, "add_label_ids", "", kMaxThreadIds).value_or(std::vector<int64_t>{});
  p.remove_label_ids = get_ids(o, "remove_label_ids", "", kMaxThreadIds).value_or(std::vector<int64_t>{});
  return p;
}

ThreadActionRequest parse_thread_action_request(const json::object& o) {
  ThreadActionRequest r;
  auto ids = get_ids(o, "thread_ids", "", kMaxThreadIds);
  if (!ids) throw_invalid_field("thread_ids");
  r.thread_ids = std::move(*ids);
  const auto action = get_string(o, "action", "");
  if (!action) throw_invalid_field("action");
  const auto parsed = parse_thread_action(*action);
  if (!parsed) throw_invalid_field("action");
  r.action = *parsed;
  r.label_id = get_int(o, "label_id", "");
  if (needs_label(r.action) && !r.label_id) throw_invalid_field("label_id");
  return r;
}

int64_t parse_reschedule(const json::object& o) {
  const auto at = get_int(o, "scheduled_at", "");
  if (!at) throw_invalid_field("scheduled_at");
  return *at;
}

}  // namespace azm::mail
