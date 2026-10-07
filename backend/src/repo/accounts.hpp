// Owner: WP-D
//
// Accounts repository (DESIGN §3 "repo/accounts", D2, C3, C4): users, sessions, addresses /
// aliases / members, domains, user settings, labels, audit log and admin read models.
// Conventions:
//  * Reads take db::Conn&, writes db::Tx& (inside Pool::write). No network, no scrypt inside
//    transactions: callers hash passwords (crypto::password_hash under the LoginThrottle scrypt
//    permit) BEFORE opening the write transaction and pass the encoded hash.
//  * Emails are normalized with normalize_email (lowercase, trimmed; +tag NOT stripped for
//    stored addresses). Address uniqueness spans users and aliases (single namespace).
//  * Errors are ApiError with the codes listed per function (docs/CONTRACTS.md "Error codes").
//  * WS revocation is not done here (no Notifier): functions return what was revoked and the
//    handler calls Notifier::revoke_session / revoke_user after COMMIT. WS hints that are
//    plain data changes (labels.changed, settings.changed) are emitted via tx.emit.
//  * Labels are per owner: every label function takes `owner` and filters on it (IDOR).
#pragma once

#include "db/sqlite.hpp"

#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::repo {

// =============================================================================================
// Users
// =============================================================================================

struct User {
  int64_t id = 0;
  std::string email;          // login address == the user's mailbox address
  std::string display_name;
  std::string password_hash;  // "$scrypt$…" (never serialized)
  bool is_admin = false;
  bool disabled = false;
  int64_t password_changed_at = 0;
  std::optional<int64_t> last_login_at;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};

struct NewUser {
  std::string email;           // must be a valid address in an existing domain
  std::string display_name;
  std::string password_hash;   // pre-computed
  bool is_admin = false;
  // Initial user_settings.undo_send_seconds; nullopt → schema default (5). `azmail create-user`
  // and POST /api/admin/users pass cfg.default_undo_send_seconds.
  std::optional<int> undo_send_seconds;
};

struct UserPatch {
  std::optional<std::string> display_name;
  std::optional<bool> is_admin;
  std::optional<bool> disabled;
  std::optional<std::string> password_hash;  // pre-computed; bumps password_changed_at
};

// Inserts users + addresses(kind 'user') + user_settings (defaults; undo_send_seconds from
// `u` when set) rows.
// Errors: 400 "invalid_field" {field:"email"} (invalid address); 400 "invalid_field"
// {field:"undo_send_seconds"} (not 0|5|10|20|30); 422 "unknown_domain" (domain not in
// `domains`); 409 "address_exists" (user or alias already has the address).
User create_user(db::Tx& tx, const NewUser& u, int64_t now_ms);

std::optional<User> get_user(db::Conn& c, int64_t user_id);
std::optional<User> find_user_by_email(db::Conn& c, std::string_view email);  // normalized match

// Applies a patch (display_name also updates the user's address display_name and emits
// settings.changed to the user). Disabling a user deletes all their sessions in the same
// transaction (the handler then calls Notifier::revoke_user). A new password_hash does NOT
// revoke sessions (the handler decides: revoke_all_sessions). Errors: 404 "not_found";
// 409 "last_admin" when the change would leave
// no active admin; 400 "invalid_field" {field:"display_name"} (> 100 chars).
User update_user(db::Tx& tx, int64_t user_id, const UserPatch& p, int64_t now_ms);

// Deletes the user (cascades sessions, settings, labels, mail). Errors: 404 "not_found";
// 409 "cannot_delete_self" when user_id == acting_user_id; 409 "last_admin".
void delete_user(db::Tx& tx, int64_t user_id, int64_t acting_user_id);

// users.last_login_at = now.
void record_login(db::Tx& tx, int64_t user_id, int64_t now_ms);

// Active (not disabled) admins.
int64_t count_active_admins(db::Conn& c);

// =============================================================================================
// Sessions
// =============================================================================================

struct Session {
  int64_t id = 0;
  int64_t user_id = 0;
  int64_t created_at = 0;
  int64_t last_seen_at = 0;
  int64_t expires_at = 0;
  std::string user_agent;  // truncated to 256 bytes
  std::string ip;
};

struct CreatedSession {
  std::string token;  // raw bearer token (crypto::random_token_b64url(32)); returned ONCE, never stored
  Session session;
};

// Stores sha256(token) (BLOB) with expires_at = now + ttl_ms.
CreatedSession create_session(db::Tx& tx, int64_t user_id, int64_t ttl_ms, std::string_view user_agent,
                              std::string_view ip, int64_t now_ms);

struct AuthSession {
  Session session;
  User user;
  bool needs_touch = false;  // last_seen_at older than touch_interval_ms
};

// Lookup by sha256(raw_token). nullopt when unknown, expired (expires_at <= now) or the user is
// disabled. Read-only (callers touch separately).
std::optional<AuthSession> find_session(db::Conn& c, std::string_view raw_token, int64_t now_ms,
                                        int64_t touch_interval_ms = 3600 * 1000);

// Sliding expiry: last_seen_at = now, expires_at = now + ttl_ms. False when the session is gone.
bool touch_session(db::Tx& tx, int64_t session_id, int64_t now_ms, int64_t ttl_ms);

// Deletes one session (logout). False when already gone.
bool revoke_session(db::Tx& tx, int64_t session_id);

// Deletes all sessions of a user except `except_session_id`; returns the deleted session ids
// (for Notifier::revoke_session). Password change keeps the current one; disable/reset none.
std::vector<int64_t> revoke_all_sessions(db::Tx& tx, int64_t user_id,
                                         std::optional<int64_t> except_session_id = std::nullopt);

// Deletes expired sessions (gc.housekeeping). Returns rows deleted.
int purge_expired_sessions(db::Tx& tx, int64_t now_ms);

// sha256 of the raw token (32 raw bytes) as stored in sessions.token_hash.
std::string session_token_hash(std::string_view raw_token);

// =============================================================================================
// Addresses, aliases, identities
// =============================================================================================

enum class AddressKind { User, Alias };
inline constexpr std::string_view to_string(AddressKind k) { return k == AddressKind::Alias ? "alias" : "user"; }

struct AddressRow {
  int64_t id = 0;
  std::string email;
  int64_t domain_id = 0;
  AddressKind kind = AddressKind::User;
  std::optional<int64_t> user_id;  // kind User only
  std::string display_name;
  bool share_sent = true;          // aliases: shared sent copies for members (C3)
  int64_t created_at = 0;
};

// Exact normalized match (no +tag stripping; routing is mail::resolve_local_recipients).
std::optional<AddressRow> find_address(db::Conn& c, std::string_view email);
std::optional<AddressRow> get_address(db::Conn& c, int64_t address_id);
// The user's own mailbox address.
std::optional<AddressRow> user_address(db::Conn& c, int64_t user_id);

struct AliasMember {  // AdminAlias.members[]
  int64_t user_id = 0;
  std::string email;
  std::string display_name;
  bool can_send_as = false;
};

struct Alias {  // AdminAlias
  int64_t id = 0;  // addresses.id
  std::string email;
  std::string display_name;
  bool share_sent = true;
  int64_t created_at = 0;
  std::vector<AliasMember> members;  // by user email
};

struct AliasMemberInput {
  int64_t user_id = 0;
  bool can_send_as = false;
};

struct AliasInput {  // POST /api/admin/aliases
  std::string email;
  std::string display_name;
  bool share_sent = true;
  std::vector<AliasMemberInput> members;
};

struct AliasPatch {  // PATCH /api/admin/aliases/:id
  std::optional<std::string> email;
  std::optional<std::string> display_name;
  std::optional<bool> share_sent;
  std::optional<std::vector<AliasMemberInput>> members;  // replaced wholesale when present
};

std::vector<Alias> list_aliases(db::Conn& c);  // by email
std::optional<Alias> get_alias(db::Conn& c, int64_t alias_id);
// Errors: 400 "invalid_field" {field:"email"}; 422 "unknown_domain"; 409 "address_exists";
// 400 "invalid_field" {field:"members"} (unknown / duplicate user id).
Alias create_alias(db::Tx& tx, const AliasInput& in, int64_t now_ms);
// Errors: 404 "not_found" and the create errors.
Alias update_alias(db::Tx& tx, int64_t alias_id, const AliasPatch& p);
// Errors: 404 "not_found"; 409 "alias_in_use" when outbound rows reference it
// (outbound.from_address_id is NOT NULL with no ON DELETE action, so the DELETE would fail with
// a FOREIGN KEY error) — remove the members instead. Checked explicitly before deleting.
// messages.from_address_id is ON DELETE SET NULL (drafts fall back to the owner's address).
void delete_alias(db::Tx& tx, int64_t alias_id);

struct Identity {  // API Identity
  int64_t address_id = 0;
  std::string email;
  std::string display_name;  // alias display name, or the user's display_name for their mailbox
  AddressKind kind = AddressKind::User;
  bool is_default = false;   // the user's own mailbox address
};
// The user's own address first (is_default), then aliases with can_send_as=1 by email. Same
// send-as rule as mail::resolve_sender (which enforces it at send time).
std::vector<Identity> identities_for_user(db::Conn& c, int64_t user_id);
// Send-as check by that same rule (UI / API validation; mail::resolve_sender is authoritative).
bool can_send_as(db::Conn& c, int64_t user_id, int64_t address_id);

// =============================================================================================
// Domains
// =============================================================================================

struct Domain {  // API DomainRow
  int64_t id = 0;
  std::string name;  // lowercase
  bool receiving_enabled = true;
  int64_t created_at = 0;
};

std::vector<Domain> list_domains(db::Conn& c);  // by name
std::optional<Domain> get_domain(db::Conn& c, int64_t domain_id);
std::optional<Domain> find_domain(db::Conn& c, std::string_view name);  // case-insensitive
bool is_local_domain(db::Conn& c, std::string_view name);
// Errors: 400 "invalid_field" {field:"name"} (not a valid domain); 409 "domain_exists".
Domain add_domain(db::Tx& tx, std::string_view name, int64_t now_ms);
// Errors: 404 "not_found"; 409 "domain_in_use" when addresses reference it.
void remove_domain(db::Tx& tx, int64_t domain_id);

// =============================================================================================
// User settings
// =============================================================================================

enum class RemoteImages { Ask, Always };
inline constexpr std::string_view to_string(RemoteImages r) { return r == RemoteImages::Always ? "always" : "ask"; }

struct UserSettings {  // API Settings
  int undo_send_seconds = 5;         // 0 | 5 | 10 | 20 | 30
  std::string signature_html;        // sanitized-on-display HTML (stored as given)
  bool signature_enabled = true;
  std::string timezone = "Asia/Shanghai";  // IANA name, display only
  int page_size = 50;                // 10..100
  RemoteImages remote_images = RemoteImages::Ask;
  std::vector<std::string> trusted_image_senders;  // normalized emails, sorted
  std::string display_name;          // users.display_name
};

struct SettingsPatch {  // PUT /api/settings (partial)
  std::optional<int> undo_send_seconds;
  std::optional<std::string> signature_html;  // ≤ 64 KiB
  std::optional<bool> signature_enabled;
  std::optional<std::string> timezone;        // 1..64 chars of [A-Za-z0-9_+/-]
  std::optional<int> page_size;
  std::optional<RemoteImages> remote_images;
  std::optional<std::vector<std::string>> trusted_image_senders;  // replaces the list (≤ 500)
  std::optional<std::string> display_name;    // 1..100 chars; also updates the user's address
};

// Settings (defaults when the row is missing).
UserSettings get_settings(db::Conn& c, int64_t user_id);
// Validates, upserts, emits settings.changed to the user, returns the full settings.
// Errors: 400 "invalid_field" {field}; 404 "not_found" (user gone).
UserSettings update_settings(db::Tx& tx, int64_t user_id, const SettingsPatch& p, int64_t now_ms);

// =============================================================================================
// Labels
// =============================================================================================

struct Label {  // API Label (+ owner/created_at)
  int64_t id = 0;
  int64_t owner_id = 0;
  std::string name;
  std::string color = "#9aa0a6";
  int64_t sort_order = 0;
  int64_t created_at = 0;
};

struct LabelInput {  // POST /api/labels
  std::string name;   // 1..64 chars after trim, no control chars
  std::string color;  // "#rrggbb"
  std::optional<int64_t> sort_order;  // default: max + 1
};

struct LabelPatch {  // PATCH /api/labels/:id
  std::optional<std::string> name;
  std::optional<std::string> color;
  std::optional<int64_t> sort_order;
};

std::vector<Label> list_labels(db::Conn& c, int64_t owner);  // by sort_order, name
std::optional<Label> get_label(db::Conn& c, int64_t owner, int64_t label_id);
// Errors: 400 "invalid_field" {field}; 409 "label_exists" (name, case-insensitive). Emits
// labels.changed.
Label create_label(db::Tx& tx, int64_t owner, const LabelInput& in, int64_t now_ms);
// Errors: 404 "not_found"; 400 "invalid_field"; 409 "label_exists". Emits labels.changed.
Label update_label(db::Tx& tx, int64_t owner, int64_t label_id, const LabelPatch& p);
// Deletes the label (message_labels cascade). Errors: 404 "not_found". Emits labels.changed and
// threads.changed for the threads that carried it.
void delete_label(db::Tx& tx, int64_t owner, int64_t label_id);

// =============================================================================================
// Audit log
// =============================================================================================

// Appends an audit_log row. `action` e.g. "user.create", "user.disable", "alias.update",
// "domain.add", "login.success", "login.failure", "password.change"; detail must not contain
// secrets or mail content.
void audit(db::Tx& tx, std::optional<int64_t> actor_user_id, std::string_view action,
           std::string_view target, const boost::json::object& detail, std::string_view ip,
           int64_t now_ms);

// =============================================================================================
// Admin read models (metadata only — admins never see bodies, D7)
// =============================================================================================

struct AdminUserAlias {  // AdminUser.aliases[]
  int64_t id = 0;        // alias address id
  std::string email;
  bool can_send_as = false;
};

struct AdminUserRow {  // AdminUser
  User user;
  int64_t message_count = 0;   // the user's messages (all folders)
  int64_t storage_bytes = 0;   // sum of the user's attachment sizes + inbound raw sizes
  std::vector<AdminUserAlias> aliases;
};
std::vector<AdminUserRow> list_users_admin(db::Conn& c);  // by email
std::optional<AdminUserRow> get_user_admin(db::Conn& c, int64_t user_id);

struct WebhookEventRow {  // API WebhookEventRow (no payload)
  int64_t id = 0;
  std::string svix_id;
  std::string type;
  std::optional<std::string> resend_email_id;
  int64_t received_at = 0;
  std::optional<int64_t> processed_at;
  std::optional<std::string> result;
};
struct WebhookEventPage {  // CursorPage<WebhookEventRow>
  std::vector<WebhookEventRow> items;  // newest first
  std::optional<std::string> next_cursor;
};
// Errors: 400 "invalid_field" {field:"cursor"}.
WebhookEventPage list_webhook_events(db::Conn& c, std::optional<std::string_view> type,
                                     std::optional<std::string_view> cursor, int limit);

struct InboundRow {  // API InboundRow
  int64_t id = 0;
  std::string resend_id;
  std::string state;   // pending | delivered | unroutable | failed
  std::string source;  // webhook | poll | admin
  std::optional<std::string> message_id_header;
  std::optional<std::string> from_email;
  std::optional<std::string> subject;
  std::optional<int64_t> received_at;
  std::vector<std::string> recipients;  // parsed recipients_json
  std::optional<std::string> error;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};
// Newest first; `state` filter optional (400 "invalid_field" {field:"state"} when unknown).
std::vector<InboundRow> list_inbound(db::Conn& c, std::optional<std::string_view> state, int limit);

struct OutboxRow {  // API OutboxRow
  int64_t id = 0;
  std::string uuid;
  int64_t sender_user_id = 0;
  std::string sender_email;
  std::string from_email;
  std::string status;
  std::optional<std::string> status_detail;
  std::optional<std::string> error_name;
  std::optional<int64_t> scheduled_at;
  std::optional<std::string> scheduled_via;
  std::optional<std::string> resend_id;
  int64_t total_bytes = 0;
  std::optional<std::string> last_event;
  std::optional<int64_t> last_event_at;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};
// Newest first; `status` filter optional (400 "invalid_field" {field:"status"} when unknown).
std::vector<OutboxRow> list_outbox(db::Conn& c, std::optional<std::string_view> status, int limit);
std::optional<OutboxRow> get_outbox_row(db::Conn& c, int64_t outbound_id);

struct JobRow {  // API JobRow
  int64_t id = 0;
  std::string kind;
  std::string lane;
  int64_t priority = 0;
  boost::json::object payload;  // parsed jobs.payload
  std::string state;
  int64_t run_at = 0;
  int64_t attempts = 0;
  int64_t max_attempts = 0;
  std::optional<int64_t> locked_until;
  std::optional<std::string> dedupe_key;
  std::optional<std::string> last_error;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};
// Newest first; `state` filter optional (400 "invalid_field" {field:"state"} when unknown).
std::vector<JobRow> list_jobs(db::Conn& c, std::optional<std::string_view> state, int limit);
std::optional<JobRow> get_job(db::Conn& c, int64_t job_id);

struct StorageStats {  // AdminStats.storage (backend/delivery filled by the handler from Config)
  std::string backend;   // "local" | "r2"
  std::string delivery;  // "redirect" | "proxy"
  int64_t blob_count = 0;
  int64_t blob_bytes = 0;
};

struct AdminStats {  // API AdminStats
  int64_t users = 0;
  int64_t messages = 0;
  int64_t storage_bytes = 0;  // sum(blobs.size)
  int64_t queue_pending = 0;  // queue.pending (jobs pending + running)
  int64_t queue_dead = 0;     // queue.dead
  int64_t sent_24h = 0;       // outbound accepted in the last 24 h
  int64_t received_24h = 0;   // inbound_emails delivered in the last 24 h
  int64_t failed_24h = 0;     // outbound failed in the last 24 h
  std::optional<int64_t> last_webhook_at;  // kv last_webhook_at
  std::optional<int64_t> last_poll_at;     // kv last_poll_at
  bool quota_blocked = false;              // kv resend.quota_blocked present
  StorageStats storage;
};
AdminStats admin_stats(db::Conn& c, int64_t now_ms);

}  // namespace azm::repo
