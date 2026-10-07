// Owner: WP-D
// WP0 stub: compiles and links; WP-D implements.
#include "api/dto.hpp"

#include "core/errors.hpp"
#include "services.hpp"

namespace azm::api {

ServerInfo server_info(const Services&) { throw NotImplemented("api::server_info"); }
boost::json::object me_json(const repo::User&, const repo::UserSettings&,
                            std::span<const repo::Identity>, const ServerInfo&) {
  throw NotImplemented("api::me_json");
}
boost::json::object login_response(std::string_view, int64_t, boost::json::object) {
  throw NotImplemented("api::login_response");
}
boost::json::object health_json(std::string_view, std::string_view, std::string_view, int64_t) {
  throw NotImplemented("api::health_json");
}

boost::json::object to_json(const repo::UserSettings&) { throw NotImplemented("api::to_json(Settings)"); }
boost::json::object to_json(const repo::Identity&) { throw NotImplemented("api::to_json(Identity)"); }
boost::json::object to_json(const repo::Label&) { throw NotImplemented("api::to_json(Label)"); }
boost::json::object to_json(const repo::AdminUserRow&) { throw NotImplemented("api::to_json(AdminUser)"); }
boost::json::object to_json(const repo::Alias&) { throw NotImplemented("api::to_json(AdminAlias)"); }
boost::json::object to_json(const repo::Domain&) { throw NotImplemented("api::to_json(Domain)"); }
boost::json::object to_json(const repo::WebhookEventRow&) {
  throw NotImplemented("api::to_json(WebhookEventRow)");
}
boost::json::object to_json(const repo::WebhookEventPage&) {
  throw NotImplemented("api::to_json(WebhookEventPage)");
}
boost::json::object to_json(const repo::InboundRow&) { throw NotImplemented("api::to_json(InboundRow)"); }
boost::json::object to_json(const repo::OutboxRow&) { throw NotImplemented("api::to_json(OutboxRow)"); }
boost::json::object to_json(const repo::JobRow&) { throw NotImplemented("api::to_json(JobRow)"); }
boost::json::object to_json(const repo::AdminStats&) { throw NotImplemented("api::to_json(AdminStats)"); }
boost::json::object domain_status_json(const repo::Domain&, const std::optional<resend::DomainInfo>&) {
  throw NotImplemented("api::domain_status_json");
}

LoginRequest parse_login(const boost::json::object&) { throw NotImplemented("api::parse_login"); }
PasswordChange parse_password_change(const boost::json::object&) {
  throw NotImplemented("api::parse_password_change");
}
void validate_new_password(std::string_view) { throw NotImplemented("api::validate_new_password"); }
repo::SettingsPatch parse_settings_patch(const boost::json::object&) {
  throw NotImplemented("api::parse_settings_patch");
}
repo::LabelInput parse_label_input(const boost::json::object&) {
  throw NotImplemented("api::parse_label_input");
}
repo::LabelPatch parse_label_patch(const boost::json::object&) {
  throw NotImplemented("api::parse_label_patch");
}
AdminUserCreate parse_admin_user_create(const boost::json::object&) {
  throw NotImplemented("api::parse_admin_user_create");
}
AdminUserPatchInput parse_admin_user_patch(const boost::json::object&) {
  throw NotImplemented("api::parse_admin_user_patch");
}
repo::AliasInput parse_alias_input(const boost::json::object&) {
  throw NotImplemented("api::parse_alias_input");
}
repo::AliasPatch parse_alias_patch(const boost::json::object&) {
  throw NotImplemented("api::parse_alias_patch");
}
std::string parse_domain_input(const boost::json::object&) {
  throw NotImplemented("api::parse_domain_input");
}

}  // namespace azm::api
