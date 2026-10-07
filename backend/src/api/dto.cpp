// Owner: WP-D
//
// JSON for accounts / settings / admin types and request parsers (see dto.hpp). Output shapes
// follow docs/API.md (+ Addendum B) field by field: optionals are null (never omitted) except
// DomainDnsRecord.priority, TEXT JSON columns are parsed values.
#include "api/dto.hpp"

#include "core/errors.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "services.hpp"

#include <boost/url/parse.hpp>

#include <algorithm>
#include <limits>
#include <utility>

#ifndef AZMAIL_VERSION
#define AZMAIL_VERSION "0.0.0"
#endif

namespace azm::api {

namespace json = boost::json;

namespace {

template <class T>
json::value opt(const std::optional<T>& v) {
  if (!v) return nullptr;
  return json::value(*v);
}

// "scheme://host[:port]" of an absolute URL (lowercased); the input minus trailing '/' when it
// does not parse.
std::string origin_of(std::string_view url) {
  auto r = boost::urls::parse_absolute_uri(boost::core::string_view(url.data(), url.size()));
  if (!r || !r->has_authority() || r->encoded_host().empty()) {
    std::string s(trim(url));
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
  }
  std::string o = to_lower_ascii(std::string_view(r->scheme().data(), r->scheme().size()));
  o += "://";
  auto host = r->encoded_host_and_port();
  o += to_lower_ascii(std::string_view(host.data(), host.size()));
  return o;
}

int to_int(int64_t v, std::string_view field) {
  if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
    throw_invalid_field(field);
  return static_cast<int>(v);
}

std::optional<int> opt_int(const json::object& o, std::string_view key) {
  auto v = opt_int64(o, key);
  if (!v) return std::nullopt;
  return to_int(*v, key);
}

std::vector<repo::AliasMemberInput> parse_members(const json::array& a) {
  std::vector<repo::AliasMemberInput> out;
  out.reserve(a.size());
  for (const auto& el : a) {
    if (!el.is_object()) throw_invalid_field("members");
    const auto& m = el.as_object();
    repo::AliasMemberInput in;
    const json::value* uid = m.if_contains("user_id");
    auto id = uid ? as_int64(*uid) : std::nullopt;
    if (!id || *id <= 0) throw_invalid_field("members");
    in.user_id = *id;
    if (const json::value* c = m.if_contains("can_send_as"); c && !c->is_null()) {
      if (!c->is_bool()) throw_invalid_field("members");
      in.can_send_as = c->as_bool();
    }
    out.push_back(in);
  }
  return out;
}

}  // namespace

// ---- output ---------------------------------------------------------------------------------

ServerInfo server_info(const Services& svc) {
  ServerInfo s;
  auto add = [&](std::string o) {
    if (o.empty()) return;
    if (std::find(s.files_origins.begin(), s.files_origins.end(), o) == s.files_origins.end())
      s.files_origins.push_back(std::move(o));
  };
  add(origin_of(svc.cfg.public_api_base_url));
  for (const auto& o : svc.blobs.public_origins()) add(origin_of(o));
  // Mixed store (Addendum A, blobs-migrate): files may also be served from the other backend.
  if (svc.secondary_blobs != nullptr)
    for (const auto& o : svc.secondary_blobs->public_origins()) add(origin_of(o));
  s.blob_backend = std::string(svc.blobs.kind());
  s.version = AZMAIL_VERSION;
  return s;
}

json::object me_json(const repo::User& u, const repo::UserSettings& s,
                     std::span<const repo::Identity> identities, const ServerInfo& server) {
  json::array origins;
  for (const auto& o : server.files_origins) origins.emplace_back(o);
  json::object srv;
  srv["files_origins"] = std::move(origins);
  srv["blob_backend"] = server.blob_backend;
  srv["version"] = server.version;

  json::object me;
  me["id"] = u.id;
  me["email"] = u.email;
  me["display_name"] = u.display_name;
  me["is_admin"] = u.is_admin;
  me["settings"] = to_json(s);
  me["identities"] = to_json_array<repo::Identity>(identities);
  me["server"] = std::move(srv);
  return me;
}

json::object login_response(std::string_view token, int64_t expires_at, json::object me) {
  json::object o;
  o["token"] = token;
  o["expires_at"] = expires_at;
  o["user"] = std::move(me);
  return o;
}

json::object health_json(std::string_view status, std::string_view version, std::string_view db,
                         int64_t time_ms) {
  json::object o;
  o["status"] = status;
  o["version"] = version;
  o["db"] = db;
  o["time"] = time_ms;
  return o;
}

json::object to_json(const repo::UserSettings& s) {
  json::array trusted;
  for (const auto& e : s.trusted_image_senders) trusted.emplace_back(e);
  json::object o;
  o["undo_send_seconds"] = s.undo_send_seconds;
  o["signature_html"] = s.signature_html;
  o["signature_enabled"] = s.signature_enabled;
  o["timezone"] = s.timezone;
  o["page_size"] = s.page_size;
  o["remote_images"] = repo::to_string(s.remote_images);
  o["trusted_image_senders"] = std::move(trusted);
  o["display_name"] = s.display_name;
  return o;
}

json::object to_json(const repo::Identity& i) {
  json::object o;
  o["address_id"] = i.address_id;
  o["email"] = i.email;
  o["display_name"] = i.display_name;
  o["kind"] = repo::to_string(i.kind);
  o["is_default"] = i.is_default;
  return o;
}

json::object to_json(const repo::Label& l) {
  json::object o;
  o["id"] = l.id;
  o["name"] = l.name;
  o["color"] = l.color;
  o["sort_order"] = l.sort_order;
  return o;
}

json::object to_json(const repo::AdminUserRow& r) {
  json::array aliases;
  for (const auto& a : r.aliases) {
    json::object x;
    x["id"] = a.id;
    x["email"] = a.email;
    x["can_send_as"] = a.can_send_as;
    aliases.emplace_back(std::move(x));
  }
  json::object o;
  o["id"] = r.user.id;
  o["email"] = r.user.email;
  o["display_name"] = r.user.display_name;
  o["is_admin"] = r.user.is_admin;
  o["disabled"] = r.user.disabled;
  o["created_at"] = r.user.created_at;
  o["last_login_at"] = opt(r.user.last_login_at);
  o["message_count"] = r.message_count;
  o["storage_bytes"] = r.storage_bytes;
  o["aliases"] = std::move(aliases);
  return o;
}

json::object to_json(const repo::Alias& a) {
  json::array members;
  for (const auto& m : a.members) {
    json::object x;
    x["user_id"] = m.user_id;
    x["email"] = m.email;
    x["display_name"] = m.display_name;
    x["can_send_as"] = m.can_send_as;
    members.emplace_back(std::move(x));
  }
  json::object o;
  o["id"] = a.id;
  o["email"] = a.email;
  o["display_name"] = a.display_name;
  o["share_sent"] = a.share_sent;
  o["created_at"] = a.created_at;
  o["members"] = std::move(members);
  return o;
}

json::object to_json(const repo::Domain& d) {
  json::object o;
  o["id"] = d.id;
  o["name"] = d.name;
  o["receiving_enabled"] = d.receiving_enabled;
  o["created_at"] = d.created_at;
  return o;
}

json::object to_json(const repo::WebhookEventRow& e) {
  json::object o;
  o["id"] = e.id;
  o["svix_id"] = e.svix_id;
  o["type"] = e.type;
  o["resend_email_id"] = opt(e.resend_email_id);
  o["received_at"] = e.received_at;
  o["processed_at"] = opt(e.processed_at);
  o["result"] = opt(e.result);
  return o;
}

json::object to_json(const repo::WebhookEventPage& p) {
  json::object o;
  o["items"] = to_json_array<repo::WebhookEventRow>(p.items);
  o["next_cursor"] = opt(p.next_cursor);
  return o;
}

json::object to_json(const repo::InboundRow& r) {
  json::array rcpts;
  for (const auto& e : r.recipients) rcpts.emplace_back(e);
  json::object o;
  o["id"] = r.id;
  o["resend_id"] = r.resend_id;
  o["state"] = r.state;
  o["source"] = r.source;
  o["message_id_header"] = opt(r.message_id_header);
  o["from_email"] = opt(r.from_email);
  o["subject"] = opt(r.subject);
  o["received_at"] = opt(r.received_at);
  o["recipients"] = std::move(rcpts);
  o["error"] = opt(r.error);
  o["created_at"] = r.created_at;
  o["updated_at"] = r.updated_at;
  return o;
}

json::object to_json(const repo::OutboxRow& r) {
  json::object o;
  o["id"] = r.id;
  o["uuid"] = r.uuid;
  o["sender_user_id"] = r.sender_user_id;
  o["sender_email"] = r.sender_email;
  o["from_email"] = r.from_email;
  o["status"] = r.status;
  o["status_detail"] = opt(r.status_detail);
  o["error_name"] = opt(r.error_name);
  o["scheduled_at"] = opt(r.scheduled_at);
  o["scheduled_via"] = opt(r.scheduled_via);
  o["resend_id"] = opt(r.resend_id);
  o["total_bytes"] = r.total_bytes;
  o["last_event"] = opt(r.last_event);
  o["last_event_at"] = opt(r.last_event_at);
  o["created_at"] = r.created_at;
  o["updated_at"] = r.updated_at;
  return o;
}

json::object to_json(const repo::JobRow& r) {
  json::object o;
  o["id"] = r.id;
  o["kind"] = r.kind;
  o["lane"] = r.lane;
  o["priority"] = r.priority;
  o["payload"] = r.payload;
  o["state"] = r.state;
  o["run_at"] = r.run_at;
  o["attempts"] = r.attempts;
  o["max_attempts"] = r.max_attempts;
  o["locked_until"] = opt(r.locked_until);
  o["dedupe_key"] = opt(r.dedupe_key);
  o["last_error"] = opt(r.last_error);
  o["created_at"] = r.created_at;
  o["updated_at"] = r.updated_at;
  return o;
}

json::object to_json(const repo::AdminStats& s) {
  json::object queue;
  queue["pending"] = s.queue_pending;
  queue["dead"] = s.queue_dead;
  json::object storage;
  storage["backend"] = s.storage.backend;
  storage["delivery"] = s.storage.delivery;
  storage["blob_count"] = s.storage.blob_count;
  storage["blob_bytes"] = s.storage.blob_bytes;
  json::object o;
  o["users"] = s.users;
  o["messages"] = s.messages;
  o["storage_bytes"] = s.storage_bytes;
  o["queue"] = std::move(queue);
  o["sent_24h"] = s.sent_24h;
  o["received_24h"] = s.received_24h;
  o["failed_24h"] = s.failed_24h;
  o["last_webhook_at"] = opt(s.last_webhook_at);
  o["last_poll_at"] = opt(s.last_poll_at);
  o["quota_blocked"] = s.quota_blocked;
  o["storage"] = std::move(storage);
  return o;
}

json::object domain_status_json(const repo::Domain& d, const std::optional<resend::DomainInfo>& info) {
  json::object o;
  o["id"] = d.id;
  o["name"] = d.name;
  if (!info) {
    o["resend"] = nullptr;
    return o;
  }
  json::array records;
  for (const auto& r : info->records) {
    json::object x;
    x["record"] = r.record;
    x["name"] = r.name;
    x["type"] = r.type;
    x["ttl"] = r.ttl;
    x["status"] = r.status;
    x["value"] = r.value;
    if (r.priority) x["priority"] = *r.priority;  // omitted when absent (priority?: number)
    records.emplace_back(std::move(x));
  }
  json::object rs;
  rs["id"] = info->id;
  rs["status"] = info->status;
  rs["region"] = opt(info->region);
  rs["created_at"] = opt(info->created_at_ms);
  rs["records"] = std::move(records);
  o["resend"] = std::move(rs);
  return o;
}

// ---- request parsing ---------------------------------------------------------------------------

LoginRequest parse_login(const json::object& o) {
  LoginRequest r;
  r.email = req_string(o, "email");
  r.password = req_string(o, "password");
  return r;
}

PasswordChange parse_password_change(const json::object& o) {
  PasswordChange r;
  r.current_password = req_string(o, "current_password");
  r.new_password = req_string(o, "new_password");
  return r;
}

void validate_new_password(std::string_view password) {
  const bool blank = std::all_of(password.begin(), password.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  });
  if (password.size() < 8 || password.size() > 256 || blank) {
    json::object d;
    d["min_length"] = 8;
    d["max_length"] = 256;
    throw ApiError::unprocessable("weak_password", "密码长度需为 8–256 个字符，且不能全为空白", std::move(d));
  }
}

repo::SettingsPatch parse_settings_patch(const json::object& o) {
  repo::SettingsPatch p;
  p.undo_send_seconds = opt_int(o, "undo_send_seconds");
  p.signature_html = opt_string(o, "signature_html");
  p.signature_enabled = opt_bool(o, "signature_enabled");
  p.timezone = opt_string(o, "timezone");
  p.page_size = opt_int(o, "page_size");
  if (auto r = opt_string(o, "remote_images")) {
    if (*r == "ask") p.remote_images = repo::RemoteImages::Ask;
    else if (*r == "always") p.remote_images = repo::RemoteImages::Always;
    else throw_invalid_field("remote_images");
  }
  p.trusted_image_senders = opt_string_array(o, "trusted_image_senders");
  p.display_name = opt_string(o, "display_name");
  return p;
}

repo::LabelInput parse_label_input(const json::object& o) {
  repo::LabelInput in;
  in.name = req_string(o, "name");
  in.color = req_string(o, "color");
  in.sort_order = opt_int64(o, "sort_order");
  return in;
}

repo::LabelPatch parse_label_patch(const json::object& o) {
  repo::LabelPatch p;
  p.name = opt_string(o, "name");
  p.color = opt_string(o, "color");
  p.sort_order = opt_int64(o, "sort_order");
  return p;
}

AdminUserCreate parse_admin_user_create(const json::object& o) {
  AdminUserCreate c;
  c.email = req_string(o, "email");
  c.display_name = opt_string(o, "display_name").value_or("");
  c.password = req_string(o, "password");
  c.is_admin = opt_bool(o, "is_admin").value_or(false);
  return c;
}

AdminUserPatchInput parse_admin_user_patch(const json::object& o) {
  AdminUserPatchInput p;
  p.display_name = opt_string(o, "display_name");
  p.is_admin = opt_bool(o, "is_admin");
  p.disabled = opt_bool(o, "disabled");
  p.password = opt_string(o, "password");
  return p;
}

repo::AliasInput parse_alias_input(const json::object& o) {
  repo::AliasInput in;
  in.email = req_string(o, "email");
  in.display_name = opt_string(o, "display_name").value_or("");
  in.share_sent = opt_bool(o, "share_sent").value_or(true);
  if (const auto* m = opt_array(o, "members")) in.members = parse_members(*m);
  return in;
}

repo::AliasPatch parse_alias_patch(const json::object& o) {
  repo::AliasPatch p;
  p.email = opt_string(o, "email");
  p.display_name = opt_string(o, "display_name");
  p.share_sent = opt_bool(o, "share_sent");
  if (const auto* m = opt_array(o, "members")) p.members = parse_members(*m);
  return p;
}

std::string parse_domain_input(const json::object& o) { return req_string(o, "name"); }

}  // namespace azm::api
