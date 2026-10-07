// Owner: WP-D
//
// JSON for accounts / settings / admin types (docs/API.md shapes + the frontend
// "API.md Addendum B" conventions in frontend/src/api/types.ts): optional → null (never
// omitted), TEXT JSON columns as parsed values, unpaged lists as bare arrays. Parsers throw
// ApiError(400, "invalid_json" | "invalid_field" {field}).
#pragma once

#include "repo/accounts.hpp"
#include "resend/types.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azm {
struct Services;
}

namespace azm::api {

// Me.server (Addendum A).
struct ServerInfo {
  std::vector<std::string> files_origins;  // API origin (scheme://host[:port] of cfg.public_api_base_url) + BlobStore::public_origins()
  std::string blob_backend;                // "local" | "r2" (BlobStore::kind())
  std::string version;                     // AZMAIL_VERSION
};
ServerInfo server_info(const Services& svc);

// Me {id, email, display_name, is_admin, settings, identities, server}.
boost::json::object me_json(const repo::User& u, const repo::UserSettings& s,
                            std::span<const repo::Identity> identities, const ServerInfo& server);
// {token, expires_at, user: Me}
boost::json::object login_response(std::string_view token, int64_t expires_at, boost::json::object me);
// {status, version, db, time}
boost::json::object health_json(std::string_view status, std::string_view version,
                                std::string_view db, int64_t time_ms);

boost::json::object to_json(const repo::UserSettings& s);   // Settings
boost::json::object to_json(const repo::Identity& i);       // Identity
boost::json::object to_json(const repo::Label& l);          // Label {id, name, color, sort_order}
boost::json::object to_json(const repo::AdminUserRow& u);   // AdminUser
boost::json::object to_json(const repo::Alias& a);          // AdminAlias
boost::json::object to_json(const repo::Domain& d);         // DomainRow
boost::json::object to_json(const repo::WebhookEventRow& e);  // WebhookEventRow
boost::json::object to_json(const repo::WebhookEventPage& p);  // CursorPage<WebhookEventRow>
boost::json::object to_json(const repo::InboundRow& r);     // InboundRow
boost::json::object to_json(const repo::OutboxRow& r);      // OutboxRow
boost::json::object to_json(const repo::JobRow& r);         // JobRow
boost::json::object to_json(const repo::AdminStats& s);     // AdminStats (incl. storage, queue)

// DomainStatus {id, name, resend: {id, status, region, created_at, records[]} | null}.
// Exception to "optional → null": records[].priority is OMITTED when absent (only MX records
// have one), matching frontend DomainDnsRecord `priority?: number`.
boost::json::object domain_status_json(const repo::Domain& d,
                                       const std::optional<resend::DomainInfo>& info);

// Bare JSON array of to_json(item) (unpaged list responses).
template <class T>
boost::json::array to_json_array(std::span<const T> items) {
  boost::json::array a;
  a.reserve(items.size());
  for (const auto& it : items) a.emplace_back(to_json(it));
  return a;
}

// ---- request parsing -------------------------------------------------------------------------

struct LoginRequest {
  std::string email;
  std::string password;
};
LoginRequest parse_login(const boost::json::object& o);

struct PasswordChange {
  std::string current_password;
  std::string new_password;
};
PasswordChange parse_password_change(const boost::json::object& o);

// New-password policy: 8..256 bytes, not all whitespace → else ApiError(422, "weak_password").
void validate_new_password(std::string_view password);

repo::SettingsPatch parse_settings_patch(const boost::json::object& o);  // Partial<Settings>
repo::LabelInput parse_label_input(const boost::json::object& o);        // {name, color, sort_order?}
repo::LabelPatch parse_label_patch(const boost::json::object& o);        // Partial<LabelInput>

struct AdminUserCreate {  // POST /api/admin/users
  std::string email;
  std::string display_name;
  std::string password;  // plain; hashed by the handler outside the transaction
  bool is_admin = false;
};
AdminUserCreate parse_admin_user_create(const boost::json::object& o);

struct AdminUserPatchInput {  // PATCH /api/admin/users/:id
  std::optional<std::string> display_name;
  std::optional<bool> is_admin;
  std::optional<bool> disabled;
  std::optional<std::string> password;  // plain; hashed by the handler
};
AdminUserPatchInput parse_admin_user_patch(const boost::json::object& o);

repo::AliasInput parse_alias_input(const boost::json::object& o);  // AdminAliasInput
repo::AliasPatch parse_alias_patch(const boost::json::object& o);  // AdminAliasPatch

std::string parse_domain_input(const boost::json::object& o);  // {name} → name

}  // namespace azm::api
