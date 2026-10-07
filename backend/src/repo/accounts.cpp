// Owner: WP-D
// WP0 stub: compiles and links; WP-D implements (session_token_hash is trivial and done).
#include "repo/accounts.hpp"

#include "core/crypto.hpp"
#include "core/errors.hpp"

namespace azm::repo {

User create_user(db::Tx&, const NewUser&, int64_t) { throw NotImplemented("repo::create_user"); }
std::optional<User> get_user(db::Conn&, int64_t) { throw NotImplemented("repo::get_user"); }
std::optional<User> find_user_by_email(db::Conn&, std::string_view) {
  throw NotImplemented("repo::find_user_by_email");
}
User update_user(db::Tx&, int64_t, const UserPatch&, int64_t) {
  throw NotImplemented("repo::update_user");
}
void delete_user(db::Tx&, int64_t, int64_t) { throw NotImplemented("repo::delete_user"); }
void record_login(db::Tx&, int64_t, int64_t) { throw NotImplemented("repo::record_login"); }
int64_t count_active_admins(db::Conn&) { throw NotImplemented("repo::count_active_admins"); }

CreatedSession create_session(db::Tx&, int64_t, int64_t, std::string_view, std::string_view,
                              int64_t) {
  throw NotImplemented("repo::create_session");
}
std::optional<AuthSession> find_session(db::Conn&, std::string_view, int64_t, int64_t) {
  throw NotImplemented("repo::find_session");
}
bool touch_session(db::Tx&, int64_t, int64_t, int64_t) { throw NotImplemented("repo::touch_session"); }
bool revoke_session(db::Tx&, int64_t) { throw NotImplemented("repo::revoke_session"); }
std::vector<int64_t> revoke_all_sessions(db::Tx&, int64_t, std::optional<int64_t>) {
  throw NotImplemented("repo::revoke_all_sessions");
}
int purge_expired_sessions(db::Tx&, int64_t) { throw NotImplemented("repo::purge_expired_sessions"); }
std::string session_token_hash(std::string_view raw_token) { return crypto::sha256(raw_token); }

std::optional<AddressRow> find_address(db::Conn&, std::string_view) {
  throw NotImplemented("repo::find_address");
}
std::optional<AddressRow> get_address(db::Conn&, int64_t) { throw NotImplemented("repo::get_address"); }
std::optional<AddressRow> user_address(db::Conn&, int64_t) {
  throw NotImplemented("repo::user_address");
}
std::vector<Alias> list_aliases(db::Conn&) { throw NotImplemented("repo::list_aliases"); }
std::optional<Alias> get_alias(db::Conn&, int64_t) { throw NotImplemented("repo::get_alias"); }
Alias create_alias(db::Tx&, const AliasInput&, int64_t) { throw NotImplemented("repo::create_alias"); }
Alias update_alias(db::Tx&, int64_t, const AliasPatch&) { throw NotImplemented("repo::update_alias"); }
void delete_alias(db::Tx&, int64_t) { throw NotImplemented("repo::delete_alias"); }
std::vector<Identity> identities_for_user(db::Conn&, int64_t) {
  throw NotImplemented("repo::identities_for_user");
}
bool can_send_as(db::Conn&, int64_t, int64_t) { throw NotImplemented("repo::can_send_as"); }

std::vector<Domain> list_domains(db::Conn&) { throw NotImplemented("repo::list_domains"); }
std::optional<Domain> get_domain(db::Conn&, int64_t) { throw NotImplemented("repo::get_domain"); }
std::optional<Domain> find_domain(db::Conn&, std::string_view) {
  throw NotImplemented("repo::find_domain");
}
bool is_local_domain(db::Conn&, std::string_view) { throw NotImplemented("repo::is_local_domain"); }
Domain add_domain(db::Tx&, std::string_view, int64_t) { throw NotImplemented("repo::add_domain"); }
void remove_domain(db::Tx&, int64_t) { throw NotImplemented("repo::remove_domain"); }

UserSettings get_settings(db::Conn&, int64_t) { throw NotImplemented("repo::get_settings"); }
UserSettings update_settings(db::Tx&, int64_t, const SettingsPatch&, int64_t) {
  throw NotImplemented("repo::update_settings");
}

std::vector<Label> list_labels(db::Conn&, int64_t) { throw NotImplemented("repo::list_labels"); }
std::optional<Label> get_label(db::Conn&, int64_t, int64_t) { throw NotImplemented("repo::get_label"); }
Label create_label(db::Tx&, int64_t, const LabelInput&, int64_t) {
  throw NotImplemented("repo::create_label");
}
Label update_label(db::Tx&, int64_t, int64_t, const LabelPatch&) {
  throw NotImplemented("repo::update_label");
}
void delete_label(db::Tx&, int64_t, int64_t) { throw NotImplemented("repo::delete_label"); }

void audit(db::Tx&, std::optional<int64_t>, std::string_view, std::string_view,
           const boost::json::object&, std::string_view, int64_t) {
  throw NotImplemented("repo::audit");
}

std::vector<AdminUserRow> list_users_admin(db::Conn&) { throw NotImplemented("repo::list_users_admin"); }
std::optional<AdminUserRow> get_user_admin(db::Conn&, int64_t) {
  throw NotImplemented("repo::get_user_admin");
}
WebhookEventPage list_webhook_events(db::Conn&, std::optional<std::string_view>,
                                     std::optional<std::string_view>, int) {
  throw NotImplemented("repo::list_webhook_events");
}
std::vector<InboundRow> list_inbound(db::Conn&, std::optional<std::string_view>, int) {
  throw NotImplemented("repo::list_inbound");
}
std::vector<OutboxRow> list_outbox(db::Conn&, std::optional<std::string_view>, int) {
  throw NotImplemented("repo::list_outbox");
}
std::optional<OutboxRow> get_outbox_row(db::Conn&, int64_t) {
  throw NotImplemented("repo::get_outbox_row");
}
std::vector<JobRow> list_jobs(db::Conn&, std::optional<std::string_view>, int) {
  throw NotImplemented("repo::list_jobs");
}
std::optional<JobRow> get_job(db::Conn&, int64_t) { throw NotImplemented("repo::get_job"); }
AdminStats admin_stats(db::Conn&, int64_t) { throw NotImplemented("repo::admin_stats"); }

}  // namespace azm::repo
