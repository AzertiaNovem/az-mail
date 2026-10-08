// Owner: WP-D
//
// Accounts repository (see accounts.hpp). Every write runs inside the caller's Pool::write
// transaction and must stay re-runnable (BUSY retry): no network, no scrypt, no side effects
// besides SQL and tx.emit.
#include "repo/accounts.hpp"

#include "core/address.hpp"
#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "db/kv.hpp"
#include "mail/types.hpp"
#include "ws/events.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace azm::repo {

namespace json = boost::json;

namespace {

constexpr int64_t kDay = 24LL * 3600 * 1000;
constexpr std::size_t kMaxDisplayName = 100;  // code points
constexpr std::size_t kMaxLabelName = 64;     // code points
constexpr std::size_t kMaxSignatureBytes = 64u << 10;
constexpr std::size_t kMaxTrustedSenders = 500;
constexpr std::size_t kMaxUserAgentBytes = 256;
constexpr std::size_t kMaxIpBytes = 64;
constexpr std::size_t kMaxTokenBytes = 512;
constexpr int64_t kMaxSortOrder = 1'000'000'000;
constexpr std::string_view kDefaultLabelColor = "#9aa0a6";

// ---- errors ---------------------------------------------------------------------------------

[[noreturn]] void invalid(std::string_view field, std::string message = "参数无效") {
  json::object d;
  d["field"] = field;
  throw ApiError::bad_request("invalid_field", std::move(message), std::move(d));
}
[[noreturn]] void not_found(std::string message = "资源不存在") {
  throw ApiError::not_found("not_found", std::move(message));
}
[[noreturn]] void address_exists() {
  throw ApiError::conflict("address_exists", "该邮箱地址已被用户或别名占用");
}
[[noreturn]] void unknown_domain() {
  throw ApiError::unprocessable("unknown_domain", "该邮箱的域名不存在，请先添加域名");
}
[[noreturn]] void last_admin() {
  throw ApiError::conflict("last_admin", "至少需要保留一名启用状态的管理员");
}

// ---- validation helpers ----------------------------------------------------------------------

bool has_control_chars(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7f;
  });
}

// Trimmed display text: valid UTF-8, no control characters, at most `max_cp` code points.
std::string clean_text(std::string_view raw, std::string_view field, std::size_t max_cp,
                       bool allow_empty) {
  const std::string_view t = trim(raw);
  if (!utf8_valid(t) || has_control_chars(t)) invalid(field);
  if (t.empty() && !allow_empty) invalid(field);
  if (utf8_length(t) > max_cp) invalid(field);
  return std::string(t);
}

bool is_ldh(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// Lowercase ASCII DNS name with ≥ 2 LDH labels (1..63 chars, no leading/trailing '-'), total
// ≤ 253, and an alphabetic-or-punycode TLD. A single trailing '.' is accepted and dropped.
std::optional<std::string> normalize_domain_name(std::string_view raw) {
  std::string name = to_lower_ascii(trim(raw));
  if (!name.empty() && name.back() == '.') name.pop_back();
  if (name.empty() || name.size() > 253) return std::nullopt;
  const auto labels = split(name, '.');
  if (labels.size() < 2) return std::nullopt;
  for (const auto& l : labels) {
    if (l.empty() || l.size() > 63) return std::nullopt;
    if (l.front() == '-' || l.back() == '-') return std::nullopt;
    if (!std::all_of(l.begin(), l.end(), is_ldh)) return std::nullopt;
  }
  const auto& tld = labels.back();
  if (std::all_of(tld.begin(), tld.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;  // "1.2.3.4" is not a mail domain
  return name;
}

// A mailbox / alias address we can host: lowercase dot-atom ASCII local part without '+'
// (routing strips +tags, C1, so such an address could never receive mail) and a valid domain.
std::string checked_mailbox(std::string_view raw, std::string_view field = "email") {
  const std::string e = normalize_email(raw);
  if (!is_ascii(e) || !is_valid_email(e)) invalid(field, "邮箱地址无效");
  const std::string_view local = local_part(e);
  if (local.find('"') != std::string_view::npos) invalid(field, "邮箱地址无效");
  if (local.find('+') != std::string_view::npos) invalid(field, "邮箱地址不能包含“+”");
  if (!normalize_domain_name(domain_of(e))) invalid(field, "邮箱地址无效");
  return e;
}

bool valid_undo_seconds(int64_t v) { return v == 0 || v == 5 || v == 10 || v == 20 || v == 30; }

bool valid_timezone(std::string_view tz) {
  if (tz.empty() || tz.size() > 64) return false;
  return std::all_of(tz.begin(), tz.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '+' || c == '/' || c == '-';
  });
}

// "#rrggbb" (any case) → lowercase; nullopt otherwise.
std::optional<std::string> normalize_color(std::string_view raw) {
  const std::string_view c = trim(raw);
  if (c.size() != 7 || c[0] != '#') return std::nullopt;
  for (std::size_t i = 1; i < 7; ++i) {
    const char ch = c[i];
    const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!hex) return std::nullopt;
  }
  return to_lower_ascii(c);
}

std::vector<uint8_t> token_hash_blob(std::string_view raw_token) {
  return crypto::to_bytes(session_token_hash(raw_token));
}

std::optional<std::string> nonempty(std::string_view s) {
  if (s.empty()) return std::nullopt;
  return std::string(s);
}

// ---- row readers ------------------------------------------------------------------------------

constexpr std::string_view kUserCols =
    "u.id, u.email, u.display_name, u.password_hash, u.is_admin, u.disabled, "
    "u.password_changed_at, u.last_login_at, u.created_at, u.updated_at";

User read_user(const db::Stmt& s, int o) {
  User u;
  u.id = s.i64(o + 0);
  u.email = s.text(o + 1);
  u.display_name = s.text(o + 2);
  u.password_hash = s.text(o + 3);
  u.is_admin = s.boolean(o + 4);
  u.disabled = s.boolean(o + 5);
  u.password_changed_at = s.i64(o + 6);
  u.last_login_at = s.opt_i64(o + 7);
  u.created_at = s.i64(o + 8);
  u.updated_at = s.i64(o + 9);
  return u;
}

constexpr std::string_view kSessionCols =
    "s.id, s.user_id, s.created_at, s.last_seen_at, s.expires_at, s.user_agent, s.ip";

Session read_session(const db::Stmt& s, int o) {
  Session r;
  r.id = s.i64(o + 0);
  r.user_id = s.i64(o + 1);
  r.created_at = s.i64(o + 2);
  r.last_seen_at = s.i64(o + 3);
  r.expires_at = s.i64(o + 4);
  r.user_agent = s.text(o + 5);
  r.ip = s.text(o + 6);
  return r;
}

constexpr std::string_view kAddressCols =
    "a.id, a.email, a.domain_id, a.kind, a.user_id, a.display_name, a.share_sent, a.created_at";

AddressRow read_address(const db::Stmt& s, int o) {
  AddressRow a;
  a.id = s.i64(o + 0);
  a.email = s.text(o + 1);
  a.domain_id = s.i64(o + 2);
  a.kind = s.text(o + 3) == "alias" ? AddressKind::Alias : AddressKind::User;
  a.user_id = s.opt_i64(o + 4);
  a.display_name = s.text(o + 5);
  a.share_sent = s.boolean(o + 6);
  a.created_at = s.i64(o + 7);
  return a;
}

Domain read_domain(const db::Stmt& s) {
  Domain d;
  d.id = s.i64(0);
  d.name = s.text(1);
  d.receiving_enabled = s.boolean(2);
  d.created_at = s.i64(3);
  return d;
}

Label read_label(const db::Stmt& s) {
  Label l;
  l.id = s.i64(0);
  l.owner_id = s.i64(1);
  l.name = s.text(2);
  l.color = s.text(3);
  l.sort_order = s.i64(4);
  l.created_at = s.i64(5);
  return l;
}

std::optional<json::value> parse_json_text(std::string_view text) {
  boost::system::error_code ec;
  json::value v = json::parse(text, ec);
  if (ec) return std::nullopt;
  return v;
}

// ---- shared checks ------------------------------------------------------------------------------

int64_t require_domain(db::Conn& c, std::string_view domain) {
  auto id = c.scalar<int64_t>("SELECT id FROM domains WHERE name=?", to_lower_ascii(domain));
  if (!id) unknown_domain();
  return *id;
}

// 409 address_exists unless `email` is free in the shared users/addresses namespace
// (`self_address_id` is ignored, for renames).
void ensure_address_free(db::Conn& c, std::string_view email,
                         std::optional<int64_t> self_address_id = std::nullopt) {
  auto owner = c.scalar<int64_t>("SELECT id FROM addresses WHERE email=?", email);
  if (owner && (!self_address_id || *owner != *self_address_id)) address_exists();
  if (c.scalar<int64_t>("SELECT id FROM users WHERE email=?", email)) {
    // A user's login email always equals its mailbox address, so this only triggers on drift.
    if (!self_address_id || !owner || *owner != *self_address_id) address_exists();
  }
}

// Members must be distinct existing users.
void validate_members(db::Conn& c, const std::vector<AliasMemberInput>& members) {
  std::set<int64_t> seen;
  for (const auto& m : members) {
    if (m.user_id <= 0 || !seen.insert(m.user_id).second) invalid("members", "别名成员无效或重复");
    if (!c.scalar<int64_t>("SELECT 1 FROM users WHERE id=?", m.user_id))
      invalid("members", "别名成员用户不存在");
  }
}

std::vector<int64_t> alias_member_ids(db::Conn& c, int64_t alias_id) {
  std::vector<int64_t> ids;
  auto s = c.prepare("SELECT user_id FROM alias_members WHERE alias_id=? ORDER BY user_id");
  s.bind_all(alias_id);
  while (s.step()) ids.push_back(s.i64(0));
  return ids;
}

void replace_members(db::Tx& tx, int64_t alias_id, const std::vector<AliasMemberInput>& members) {
  tx.run("DELETE FROM alias_members WHERE alias_id=?", alias_id);
  for (const auto& m : members)
    tx.run("INSERT INTO alias_members(alias_id, user_id, can_send_as) VALUES(?,?,?)", alias_id,
           m.user_id, m.can_send_as);
}

// Identities are part of Me: tell the affected users' clients to refetch it.
void emit_identities_changed(db::Tx& tx, std::set<int64_t> users) {
  for (int64_t uid : users) tx.emit(uid, std::string(ws::events::kSettingsChanged), {});
}

}  // namespace

// =============================================================================================
// Users
// =============================================================================================

User create_user(db::Tx& tx, const NewUser& u, int64_t now_ms) {
  const std::string email = checked_mailbox(u.email);
  const std::string name = clean_text(u.display_name, "display_name", kMaxDisplayName, true);
  if (u.password_hash.empty()) invalid("password");
  if (u.undo_send_seconds && !valid_undo_seconds(*u.undo_send_seconds)) invalid("undo_send_seconds");
  db::Conn& c = tx.conn();
  const int64_t domain_id = require_domain(c, domain_of(email));
  ensure_address_free(c, email);

  int64_t uid = 0;
  try {
    tx.run(
        "INSERT INTO users(email, display_name, password_hash, is_admin, disabled, "
        "password_changed_at, created_at, updated_at) VALUES(?,?,?,?,0,?,?,?)",
        email, name, u.password_hash, u.is_admin, now_ms, now_ms, now_ms);
    uid = tx.last_insert_id();
    tx.run(
        "INSERT INTO addresses(email, domain_id, kind, user_id, display_name, share_sent, "
        "created_at) VALUES(?,?,'user',?,?,1,?)",
        email, domain_id, uid, name, now_ms);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) address_exists();
    throw;
  }
  if (u.undo_send_seconds)
    tx.run("INSERT INTO user_settings(user_id, undo_send_seconds, updated_at) VALUES(?,?,?)", uid,
           *u.undo_send_seconds, now_ms);
  else
    tx.run("INSERT INTO user_settings(user_id, updated_at) VALUES(?,?)", uid, now_ms);
  return *get_user(c, uid);
}

std::optional<User> get_user(db::Conn& c, int64_t user_id) {
  auto s = c.prepare("SELECT " + std::string(kUserCols) + " FROM users u WHERE u.id=?");
  s.bind_all(user_id);
  if (!s.step()) return std::nullopt;
  return read_user(s, 0);
}

std::optional<User> find_user_by_email(db::Conn& c, std::string_view email) {
  const std::string e = normalize_email(email);
  if (e.empty()) return std::nullopt;
  auto s = c.prepare("SELECT " + std::string(kUserCols) + " FROM users u WHERE u.email=?");
  s.bind_all(e);
  if (!s.step()) return std::nullopt;
  return read_user(s, 0);
}

User update_user(db::Tx& tx, int64_t user_id, const UserPatch& p, int64_t now_ms) {
  db::Conn& c = tx.conn();
  auto cur = get_user(c, user_id);
  if (!cur) not_found("用户不存在");

  std::optional<std::string> name;
  if (p.display_name) name = clean_text(*p.display_name, "display_name", kMaxDisplayName, true);
  if (p.password_hash && p.password_hash->empty()) invalid("password");

  const bool new_admin = p.is_admin.value_or(cur->is_admin);
  const bool new_disabled = p.disabled.value_or(cur->disabled);
  const bool was_active_admin = cur->is_admin && !cur->disabled;
  if (was_active_admin && !(new_admin && !new_disabled) && count_active_admins(c) <= 1) last_admin();

  tx.run(
      "UPDATE users SET display_name=COALESCE(?, display_name), is_admin=?, disabled=?, "
      "password_hash=COALESCE(?, password_hash), "
      "password_changed_at=CASE WHEN ? IS NULL THEN password_changed_at ELSE ? END, "
      "updated_at=? WHERE id=?",
      name, new_admin, new_disabled, p.password_hash, p.password_hash, now_ms, now_ms, user_id);
  if (name)
    tx.run("UPDATE addresses SET display_name=? WHERE kind='user' AND user_id=?", *name, user_id);
  if (p.disabled.value_or(false)) tx.run("DELETE FROM sessions WHERE user_id=?", user_id);

  // Me.display_name / Me.is_admin changed: hint the user's other tabs to refetch Me.
  if ((name && *name != cur->display_name) || new_admin != cur->is_admin)
    tx.emit(user_id, std::string(ws::events::kSettingsChanged), {});
  return *get_user(c, user_id);
}

void delete_user(db::Tx& tx, int64_t user_id, int64_t acting_user_id) {
  db::Conn& c = tx.conn();
  auto u = get_user(c, user_id);
  if (!u) not_found("用户不存在");
  if (user_id == acting_user_id)
    throw ApiError::conflict("cannot_delete_self", "不能删除自己的账号");
  if (u->is_admin && !u->disabled && count_active_admins(c) <= 1) last_admin();
  // Cascades: address, alias memberships, sessions, settings, labels, threads/messages,
  // attachments rows, outbound rows sent by the user; blobs are left to gc.blobs.
  tx.run("DELETE FROM users WHERE id=?", user_id);
}

void record_login(db::Tx& tx, int64_t user_id, int64_t now_ms) {
  tx.run("UPDATE users SET last_login_at=? WHERE id=?", now_ms, user_id);
}

int64_t count_active_admins(db::Conn& c) {
  return c.scalar<int64_t>("SELECT count(*) FROM users WHERE is_admin=1 AND disabled=0").value_or(0);
}

// =============================================================================================
// Sessions
// =============================================================================================

CreatedSession create_session(db::Tx& tx, int64_t user_id, int64_t ttl_ms, std::string_view user_agent,
                              std::string_view ip, int64_t now_ms) {
  if (ttl_ms <= 0) throw std::invalid_argument("repo::create_session: ttl_ms must be > 0");
  CreatedSession out;
  out.token = crypto::random_token_b64url(32);
  Session& s = out.session;
  s.user_id = user_id;
  s.created_at = now_ms;
  s.last_seen_at = now_ms;
  s.expires_at = now_ms + ttl_ms;
  s.user_agent = utf8_truncate(utf8_sanitize(user_agent), kMaxUserAgentBytes);
  s.ip = utf8_truncate(utf8_sanitize(ip), kMaxIpBytes);
  tx.run(
      "INSERT INTO sessions(user_id, token_hash, created_at, last_seen_at, expires_at, user_agent, "
      "ip) VALUES(?,?,?,?,?,?,?)",
      user_id, token_hash_blob(out.token), s.created_at, s.last_seen_at, s.expires_at, s.user_agent,
      s.ip);
  s.id = tx.last_insert_id();
  return out;
}

std::optional<AuthSession> find_session(db::Conn& c, std::string_view raw_token, int64_t now_ms,
                                        int64_t touch_interval_ms) {
  if (raw_token.empty() || raw_token.size() > kMaxTokenBytes) return std::nullopt;
  auto s = c.prepare("SELECT " + std::string(kSessionCols) + ", " + std::string(kUserCols) +
                     " FROM sessions s JOIN users u ON u.id = s.user_id "
                     "WHERE s.token_hash=? AND s.expires_at>? AND u.disabled=0");
  s.bind_all(token_hash_blob(raw_token), now_ms);
  if (!s.step()) return std::nullopt;
  AuthSession a;
  a.session = read_session(s, 0);
  a.user = read_user(s, 7);
  a.needs_touch = now_ms - a.session.last_seen_at >= touch_interval_ms;
  return a;
}

bool touch_session(db::Tx& tx, int64_t session_id, int64_t now_ms, int64_t ttl_ms) {
  // Never resurrect a session that expired between lookup and touch.
  tx.run("UPDATE sessions SET last_seen_at=?, expires_at=? WHERE id=? AND expires_at>?", now_ms,
         now_ms + ttl_ms, session_id, now_ms);
  return tx.changes() > 0;
}

bool revoke_session(db::Tx& tx, int64_t session_id) {
  tx.run("DELETE FROM sessions WHERE id=?", session_id);
  return tx.changes() > 0;
}

std::vector<int64_t> revoke_all_sessions(db::Tx& tx, int64_t user_id,
                                         std::optional<int64_t> except_session_id) {
  std::vector<int64_t> ids;
  {
    auto s = tx.prepare("SELECT id FROM sessions WHERE user_id=? AND id IS NOT ? ORDER BY id");
    s.bind_all(user_id, except_session_id);
    while (s.step()) ids.push_back(s.i64(0));
  }
  tx.run("DELETE FROM sessions WHERE user_id=? AND id IS NOT ?", user_id, except_session_id);
  return ids;
}

int purge_expired_sessions(db::Tx& tx, int64_t now_ms) {
  tx.run("DELETE FROM sessions WHERE expires_at<=?", now_ms);
  return tx.changes();
}

std::string session_token_hash(std::string_view raw_token) { return crypto::sha256(raw_token); }

// =============================================================================================
// Addresses, aliases, identities
// =============================================================================================

std::optional<AddressRow> find_address(db::Conn& c, std::string_view email) {
  const std::string e = normalize_email(email);
  if (e.empty()) return std::nullopt;
  auto s = c.prepare("SELECT " + std::string(kAddressCols) + " FROM addresses a WHERE a.email=?");
  s.bind_all(e);
  if (!s.step()) return std::nullopt;
  return read_address(s, 0);
}

std::optional<AddressRow> get_address(db::Conn& c, int64_t address_id) {
  auto s = c.prepare("SELECT " + std::string(kAddressCols) + " FROM addresses a WHERE a.id=?");
  s.bind_all(address_id);
  if (!s.step()) return std::nullopt;
  return read_address(s, 0);
}

std::optional<AddressRow> user_address(db::Conn& c, int64_t user_id) {
  auto s = c.prepare("SELECT " + std::string(kAddressCols) +
                     " FROM addresses a WHERE a.kind='user' AND a.user_id=?");
  s.bind_all(user_id);
  if (!s.step()) return std::nullopt;
  return read_address(s, 0);
}

namespace {

// Aliases (optionally one) with their members, members ordered by user email.
std::vector<Alias> load_aliases(db::Conn& c, std::optional<int64_t> only_id) {
  std::vector<Alias> out;
  std::map<int64_t, std::size_t> index;
  {
    auto s = c.prepare(
        "SELECT id, email, display_name, share_sent, created_at FROM addresses "
        "WHERE kind='alias' AND (? IS NULL OR id=?) ORDER BY email");
    s.bind_all(only_id, only_id);
    while (s.step()) {
      Alias a;
      a.id = s.i64(0);
      a.email = s.text(1);
      a.display_name = s.text(2);
      a.share_sent = s.boolean(3);
      a.created_at = s.i64(4);
      index[a.id] = out.size();
      out.push_back(std::move(a));
    }
  }
  if (out.empty()) return out;
  auto s = c.prepare(
      "SELECT m.alias_id, u.id, u.email, u.display_name, m.can_send_as "
      "FROM alias_members m JOIN users u ON u.id = m.user_id "
      "JOIN addresses a ON a.id = m.alias_id "
      "WHERE a.kind='alias' AND (? IS NULL OR m.alias_id=?) ORDER BY u.email");
  s.bind_all(only_id, only_id);
  while (s.step()) {
    auto it = index.find(s.i64(0));
    if (it == index.end()) continue;
    AliasMember m;
    m.user_id = s.i64(1);
    m.email = s.text(2);
    m.display_name = s.text(3);
    m.can_send_as = s.boolean(4);
    out[it->second].members.push_back(std::move(m));
  }
  return out;
}

}  // namespace

std::vector<Alias> list_aliases(db::Conn& c) { return load_aliases(c, std::nullopt); }

std::optional<Alias> get_alias(db::Conn& c, int64_t alias_id) {
  auto v = load_aliases(c, alias_id);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}

Alias create_alias(db::Tx& tx, const AliasInput& in, int64_t now_ms) {
  const std::string email = checked_mailbox(in.email);
  const std::string name = clean_text(in.display_name, "display_name", kMaxDisplayName, true);
  db::Conn& c = tx.conn();
  const int64_t domain_id = require_domain(c, domain_of(email));
  ensure_address_free(c, email);
  validate_members(c, in.members);
  try {
    tx.run(
        "INSERT INTO addresses(email, domain_id, kind, user_id, display_name, share_sent, "
        "created_at) VALUES(?,?,'alias',NULL,?,?,?)",
        email, domain_id, name, in.share_sent, now_ms);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) address_exists();
    throw;
  }
  const int64_t id = tx.last_insert_id();
  replace_members(tx, id, in.members);
  std::set<int64_t> affected;
  for (const auto& m : in.members) affected.insert(m.user_id);
  emit_identities_changed(tx, std::move(affected));
  return *get_alias(c, id);
}

Alias update_alias(db::Tx& tx, int64_t alias_id, const AliasPatch& p) {
  db::Conn& c = tx.conn();
  auto cur = get_alias(c, alias_id);
  if (!cur) not_found("别名不存在");

  std::optional<std::string> email;
  std::optional<int64_t> domain_id;
  if (p.email) {
    email = checked_mailbox(*p.email);
    domain_id = require_domain(c, domain_of(*email));
    ensure_address_free(c, *email, alias_id);
  }
  std::optional<std::string> name;
  if (p.display_name) name = clean_text(*p.display_name, "display_name", kMaxDisplayName, true);
  if (p.members) validate_members(c, *p.members);

  try {
    tx.run(
        "UPDATE addresses SET email=COALESCE(?, email), domain_id=COALESCE(?, domain_id), "
        "display_name=COALESCE(?, display_name), share_sent=COALESCE(?, share_sent) "
        "WHERE id=? AND kind='alias'",
        email, domain_id, name, p.share_sent, alias_id);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) address_exists();
    throw;
  }
  std::set<int64_t> affected;
  for (const auto& m : cur->members) affected.insert(m.user_id);
  if (p.members) {
    replace_members(tx, alias_id, *p.members);
    for (const auto& m : *p.members) affected.insert(m.user_id);
  }
  emit_identities_changed(tx, std::move(affected));
  return *get_alias(c, alias_id);
}

void delete_alias(db::Tx& tx, int64_t alias_id) {
  db::Conn& c = tx.conn();
  if (!c.scalar<int64_t>("SELECT 1 FROM addresses WHERE id=? AND kind='alias'", alias_id))
    not_found("别名不存在");
  if (c.scalar<int64_t>("SELECT 1 FROM outbound WHERE from_address_id=? LIMIT 1", alias_id))
    throw ApiError::conflict("alias_in_use", "该别名已有发信记录，无法删除；可以改为移除其全部成员");
  const auto members = alias_member_ids(c, alias_id);
  // alias_members cascade; drafts' messages.from_address_id is SET NULL.
  tx.run("DELETE FROM addresses WHERE id=? AND kind='alias'", alias_id);
  emit_identities_changed(tx, std::set<int64_t>(members.begin(), members.end()));
}

std::vector<Identity> identities_for_user(db::Conn& c, int64_t user_id) {
  std::vector<Identity> out;
  {
    auto s = c.prepare(
        "SELECT a.id, a.email, u.display_name FROM addresses a JOIN users u ON u.id = a.user_id "
        "WHERE a.kind='user' AND a.user_id=?");
    s.bind_all(user_id);
    if (s.step()) {
      Identity i;
      i.address_id = s.i64(0);
      i.email = s.text(1);
      i.display_name = s.text(2);
      i.kind = AddressKind::User;
      i.is_default = true;
      out.push_back(std::move(i));
    }
  }
  auto s = c.prepare(
      "SELECT a.id, a.email, a.display_name FROM alias_members m "
      "JOIN addresses a ON a.id = m.alias_id "
      "WHERE m.user_id=? AND m.can_send_as=1 AND a.kind='alias' ORDER BY a.email");
  s.bind_all(user_id);
  while (s.step()) {
    Identity i;
    i.address_id = s.i64(0);
    i.email = s.text(1);
    i.display_name = s.text(2);
    i.kind = AddressKind::Alias;
    i.is_default = false;
    out.push_back(std::move(i));
  }
  return out;
}

bool can_send_as(db::Conn& c, int64_t user_id, int64_t address_id) {
  return c
      .scalar<int64_t>(
          "SELECT 1 FROM addresses a WHERE a.id=? AND ("
          " (a.kind='user' AND a.user_id=?) OR"
          " (a.kind='alias' AND EXISTS(SELECT 1 FROM alias_members m"
          "   WHERE m.alias_id=a.id AND m.user_id=? AND m.can_send_as=1)))",
          address_id, user_id, user_id)
      .has_value();
}

// =============================================================================================
// Domains
// =============================================================================================

std::vector<Domain> list_domains(db::Conn& c) {
  std::vector<Domain> out;
  auto s = c.prepare("SELECT id, name, receiving_enabled, created_at FROM domains ORDER BY name");
  while (s.step()) out.push_back(read_domain(s));
  return out;
}

std::optional<Domain> get_domain(db::Conn& c, int64_t domain_id) {
  auto s = c.prepare("SELECT id, name, receiving_enabled, created_at FROM domains WHERE id=?");
  s.bind_all(domain_id);
  if (!s.step()) return std::nullopt;
  return read_domain(s);
}

std::optional<Domain> find_domain(db::Conn& c, std::string_view name) {
  std::string n = to_lower_ascii(trim(name));
  if (!n.empty() && n.back() == '.') n.pop_back();
  if (n.empty()) return std::nullopt;
  auto s = c.prepare("SELECT id, name, receiving_enabled, created_at FROM domains WHERE name=?");
  s.bind_all(n);
  if (!s.step()) return std::nullopt;
  return read_domain(s);
}

bool is_local_domain(db::Conn& c, std::string_view name) { return find_domain(c, name).has_value(); }

Domain add_domain(db::Tx& tx, std::string_view name, int64_t now_ms) {
  auto n = normalize_domain_name(name);
  if (!n) invalid("name", "域名格式无效");
  if (find_domain(tx.conn(), *n)) throw ApiError::conflict("domain_exists", "该域名已存在");
  try {
    tx.run("INSERT INTO domains(name, receiving_enabled, created_at) VALUES(?,1,?)", *n, now_ms);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) throw ApiError::conflict("domain_exists", "该域名已存在");
    throw;
  }
  return *get_domain(tx.conn(), tx.last_insert_id());
}

void remove_domain(db::Tx& tx, int64_t domain_id) {
  db::Conn& c = tx.conn();
  if (!get_domain(c, domain_id)) not_found("域名不存在");
  if (c.scalar<int64_t>("SELECT 1 FROM addresses WHERE domain_id=? LIMIT 1", domain_id))
    throw ApiError::conflict("domain_in_use", "该域名下仍有用户或别名，无法删除");
  tx.run("DELETE FROM domains WHERE id=?", domain_id);
}

// =============================================================================================
// User settings
// =============================================================================================

UserSettings get_settings(db::Conn& c, int64_t user_id) {
  UserSettings out;
  {
    auto s = c.prepare(
        "SELECT undo_send_seconds, signature_html, signature_enabled, timezone, page_size, "
        "remote_images FROM user_settings WHERE user_id=?");
    s.bind_all(user_id);
    if (s.step()) {
      out.undo_send_seconds = static_cast<int>(s.i64(0));
      out.signature_html = s.text(1);
      out.signature_enabled = s.boolean(2);
      out.timezone = s.text(3);
      out.page_size = static_cast<int>(s.i64(4));
      out.remote_images = s.text(5) == "always" ? RemoteImages::Always : RemoteImages::Ask;
    }
  }
  out.display_name =
      c.scalar<std::string>("SELECT display_name FROM users WHERE id=?", user_id).value_or("");
  auto s = c.prepare("SELECT email FROM trusted_image_senders WHERE user_id=? ORDER BY email");
  s.bind_all(user_id);
  while (s.step()) out.trusted_image_senders.push_back(s.text(0));
  return out;
}

UserSettings update_settings(db::Tx& tx, int64_t user_id, const SettingsPatch& p, int64_t now_ms) {
  // Validate everything before writing anything.
  if (p.undo_send_seconds && !valid_undo_seconds(*p.undo_send_seconds)) invalid("undo_send_seconds");
  if (p.signature_html &&
      (p.signature_html->size() > kMaxSignatureBytes || !utf8_valid(*p.signature_html)))
    invalid("signature_html", "签名过长或包含无效字符");
  std::optional<std::string> tz;
  if (p.timezone) {
    tz = std::string(trim(*p.timezone));
    if (!valid_timezone(*tz)) invalid("timezone");
  }
  if (p.page_size && (*p.page_size < 10 || *p.page_size > 100)) invalid("page_size");
  db::Conn& c = tx.conn();
  const auto current_name = c.scalar<std::string>("SELECT display_name FROM users WHERE id=?", user_id);
  if (!current_name) not_found("用户不存在");
  std::optional<std::string> name;
  // Clients may PUT back the whole Settings they fetched: an unchanged empty name (users created
  // without one) is a no-op, any other empty name is rejected.
  if (p.display_name && !(trim(*p.display_name).empty() && current_name->empty()))
    name = clean_text(*p.display_name, "display_name", kMaxDisplayName, false);
  std::optional<std::set<std::string>> trusted;
  if (p.trusted_image_senders) {
    if (p.trusted_image_senders->size() > kMaxTrustedSenders) invalid("trusted_image_senders");
    trusted.emplace();
    for (const auto& raw : *p.trusted_image_senders) {
      std::string e = normalize_email(raw);
      if (!is_valid_email(e)) invalid("trusted_image_senders", "邮箱地址无效");
      trusted->insert(std::move(e));
    }
  }

  std::optional<std::string_view> remote;
  if (p.remote_images) remote = to_string(*p.remote_images);
  tx.run("INSERT OR IGNORE INTO user_settings(user_id, updated_at) VALUES(?,?)", user_id, now_ms);
  tx.run(
      "UPDATE user_settings SET undo_send_seconds=COALESCE(?, undo_send_seconds), "
      "signature_html=COALESCE(?, signature_html), signature_enabled=COALESCE(?, signature_enabled), "
      "timezone=COALESCE(?, timezone), page_size=COALESCE(?, page_size), "
      "remote_images=COALESCE(?, remote_images), updated_at=? WHERE user_id=?",
      p.undo_send_seconds, p.signature_html, p.signature_enabled, tz, p.page_size, remote, now_ms,
      user_id);
  if (trusted) {
    tx.run("DELETE FROM trusted_image_senders WHERE user_id=?", user_id);
    for (const auto& e : *trusted)
      tx.run("INSERT INTO trusted_image_senders(user_id, email, created_at) VALUES(?,?,?)", user_id,
             e, now_ms);
  }
  if (name) {
    tx.run("UPDATE users SET display_name=?, updated_at=? WHERE id=?", *name, now_ms, user_id);
    tx.run("UPDATE addresses SET display_name=? WHERE kind='user' AND user_id=?", *name, user_id);
  }
  tx.emit(user_id, std::string(ws::events::kSettingsChanged), {});
  return get_settings(c, user_id);
}

// =============================================================================================
// Labels
// =============================================================================================

namespace {

constexpr std::string_view kLabelCols = "id, owner_id, name, color, sort_order, created_at";

void ensure_label_name_free(db::Conn& c, int64_t owner, std::string_view name,
                            std::optional<int64_t> self_id) {
  auto id = c.scalar<int64_t>("SELECT id FROM labels WHERE owner_id=? AND name=?", owner, name);
  if (id && (!self_id || *id != *self_id))
    throw ApiError::conflict("label_exists", "已存在同名标签");
}

int64_t checked_sort_order(int64_t v) {
  if (v < -kMaxSortOrder || v > kMaxSortOrder) invalid("sort_order");
  return v;
}

}  // namespace

std::vector<Label> list_labels(db::Conn& c, int64_t owner) {
  std::vector<Label> out;
  auto s = c.prepare("SELECT " + std::string(kLabelCols) +
                     " FROM labels WHERE owner_id=? ORDER BY sort_order, name, id");
  s.bind_all(owner);
  while (s.step()) out.push_back(read_label(s));
  return out;
}

std::optional<Label> get_label(db::Conn& c, int64_t owner, int64_t label_id) {
  auto s = c.prepare("SELECT " + std::string(kLabelCols) + " FROM labels WHERE id=? AND owner_id=?");
  s.bind_all(label_id, owner);
  if (!s.step()) return std::nullopt;
  return read_label(s);
}

Label create_label(db::Tx& tx, int64_t owner, const LabelInput& in, int64_t now_ms) {
  const std::string name = clean_text(in.name, "name", kMaxLabelName, false);
  std::string color(kDefaultLabelColor);
  if (!trim(in.color).empty()) {
    auto c = normalize_color(in.color);
    if (!c) invalid("color", "颜色格式应为 #rrggbb");
    color = *c;
  }
  db::Conn& c = tx.conn();
  ensure_label_name_free(c, owner, name, std::nullopt);
  const int64_t sort_order =
      in.sort_order ? checked_sort_order(*in.sort_order)
                    : c.scalar<int64_t>("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM labels "
                                        "WHERE owner_id=?",
                                        owner)
                          .value_or(0);
  try {
    tx.run("INSERT INTO labels(owner_id, name, color, sort_order, created_at) VALUES(?,?,?,?,?)",
           owner, name, color, std::min(sort_order, kMaxSortOrder), now_ms);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) throw ApiError::conflict("label_exists", "已存在同名标签");
    throw;
  }
  const int64_t id = tx.last_insert_id();
  tx.emit(owner, std::string(ws::events::kLabelsChanged), {});
  return *get_label(c, owner, id);
}

Label update_label(db::Tx& tx, int64_t owner, int64_t label_id, const LabelPatch& p) {
  db::Conn& c = tx.conn();
  if (!get_label(c, owner, label_id)) not_found("标签不存在");
  std::optional<std::string> name;
  if (p.name) {
    name = clean_text(*p.name, "name", kMaxLabelName, false);
    ensure_label_name_free(c, owner, *name, label_id);
  }
  std::optional<std::string> color;
  if (p.color) {
    color = normalize_color(*p.color);
    if (!color) invalid("color", "颜色格式应为 #rrggbb");
  }
  std::optional<int64_t> sort_order;
  if (p.sort_order) sort_order = checked_sort_order(*p.sort_order);
  try {
    tx.run(
        "UPDATE labels SET name=COALESCE(?, name), color=COALESCE(?, color), "
        "sort_order=COALESCE(?, sort_order) WHERE id=? AND owner_id=?",
        name, color, sort_order, label_id, owner);
  } catch (const db::Error& e) {
    if (e.is_unique_violation()) throw ApiError::conflict("label_exists", "已存在同名标签");
    throw;
  }
  tx.emit(owner, std::string(ws::events::kLabelsChanged), {});
  return *get_label(c, owner, label_id);
}

void delete_label(db::Tx& tx, int64_t owner, int64_t label_id) {
  db::Conn& c = tx.conn();
  if (!get_label(c, owner, label_id)) not_found("标签不存在");
  std::vector<int64_t> thread_ids;
  {
    auto s = c.prepare(
        "SELECT DISTINCT m.thread_id FROM message_labels ml JOIN messages m ON m.id = ml.message_id "
        "WHERE ml.label_id=? AND m.owner_id=? ORDER BY m.thread_id");
    s.bind_all(label_id, owner);
    while (s.step()) thread_ids.push_back(s.i64(0));
  }
  // message_labels rows cascade. Thread aggregates carry no label data, so no recompute.
  tx.run("DELETE FROM labels WHERE id=? AND owner_id=?", label_id, owner);
  tx.emit(owner, std::string(ws::events::kLabelsChanged), {});
  if (!thread_ids.empty())
    tx.emit(owner, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(thread_ids));
}

// =============================================================================================
// Audit log
// =============================================================================================

void audit(db::Tx& tx, std::optional<int64_t> actor_user_id, std::string_view action,
           std::string_view target, const boost::json::object& detail, std::string_view ip,
           int64_t now_ms) {
  std::optional<std::string> detail_json;
  if (!detail.empty()) detail_json = json::serialize(detail);
  tx.run("INSERT INTO audit_log(actor_user_id, action, target, detail_json, ip, at) VALUES(?,?,?,?,?,?)",
         actor_user_id, action, nonempty(utf8_truncate(utf8_sanitize(target), 512)), detail_json,
         nonempty(utf8_truncate(utf8_sanitize(ip), kMaxIpBytes)), now_ms);
}

// =============================================================================================
// Admin read models
// =============================================================================================

namespace {

std::vector<AdminUserRow> load_admin_users(db::Conn& c, std::optional<int64_t> only_id) {
  std::vector<AdminUserRow> out;
  std::map<int64_t, std::size_t> index;
  {
    auto s = c.prepare("SELECT " + std::string(kUserCols) +
                       " FROM users u WHERE (? IS NULL OR u.id=?) ORDER BY u.email");
    s.bind_all(only_id, only_id);
    while (s.step()) {
      AdminUserRow r;
      r.user = read_user(s, 0);
      index[r.user.id] = out.size();
      out.push_back(std::move(r));
    }
  }
  if (out.empty()) return out;
  auto row = [&](int64_t uid) -> AdminUserRow* {
    auto it = index.find(uid);
    return it == index.end() ? nullptr : &out[it->second];
  };
  {
    auto s = c.prepare(
        "SELECT owner_id, count(*) FROM messages WHERE (? IS NULL OR owner_id=?) GROUP BY owner_id");
    s.bind_all(only_id, only_id);
    while (s.step())
      if (auto* r = row(s.i64(0))) r->message_count = s.i64(1);
  }
  // Storage = attachment rows + raw .eml blobs of the inbound mail delivered to the user (a raw
  // blob shared by several recipients counts once per recipient: it is "their" mail).
  {
    auto s = c.prepare(
        "SELECT owner_id, COALESCE(SUM(size), 0) FROM attachments "
        "WHERE (? IS NULL OR owner_id=?) GROUP BY owner_id");
    s.bind_all(only_id, only_id);
    while (s.step())
      if (auto* r = row(s.i64(0))) r->storage_bytes += s.i64(1);
  }
  {
    auto s = c.prepare(
        "SELECT x.owner_id, COALESCE(SUM(b.size), 0) FROM "
        " (SELECT DISTINCT owner_id, inbound_id FROM messages "
        "  WHERE inbound_id IS NOT NULL AND (? IS NULL OR owner_id=?)) x "
        "JOIN inbound_emails i ON i.id = x.inbound_id "
        "JOIN blobs b ON b.sha256 = i.raw_sha256 GROUP BY x.owner_id");
    s.bind_all(only_id, only_id);
    while (s.step())
      if (auto* r = row(s.i64(0))) r->storage_bytes += s.i64(1);
  }
  {
    auto s = c.prepare(
        "SELECT m.user_id, a.id, a.email, m.can_send_as FROM alias_members m "
        "JOIN addresses a ON a.id = m.alias_id "
        "WHERE a.kind='alias' AND (? IS NULL OR m.user_id=?) ORDER BY a.email");
    s.bind_all(only_id, only_id);
    while (s.step()) {
      if (auto* r = row(s.i64(0))) {
        AdminUserAlias a;
        a.id = s.i64(1);
        a.email = s.text(2);
        a.can_send_as = s.boolean(3);
        r->aliases.push_back(std::move(a));
      }
    }
  }
  return out;
}

int clamp_limit(int limit, int max) { return std::clamp(limit, 1, max); }

std::optional<int64_t> parse_cursor_id(std::string_view cursor) {
  if (cursor.empty() || cursor.size() > 19) return std::nullopt;
  int64_t v = 0;
  auto [p, ec] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), v);
  if (ec != std::errc() || p != cursor.data() + cursor.size() || v <= 0) return std::nullopt;
  return v;
}

std::vector<std::string> parse_recipients(const std::optional<std::string>& text) {
  std::vector<std::string> out;
  if (!text) return out;
  auto v = parse_json_text(*text);
  if (!v || !v->is_array()) return out;
  for (const auto& el : v->as_array()) {
    if (el.is_string()) {
      out.emplace_back(el.as_string());
    } else if (el.is_object()) {  // tolerate [{name,email}] shapes
      const auto* e = el.as_object().if_contains("email");
      if (e && e->is_string()) out.emplace_back(e->as_string());
    }
  }
  return out;
}

constexpr std::string_view kOutboxSelect =
    "SELECT o.id, o.uuid, o.sender_user_id, COALESCE(u.email, ''), COALESCE(a.email, ''), o.status, "
    "o.status_detail, o.error_name, o.scheduled_at, o.scheduled_via, o.resend_id, o.total_bytes, "
    "o.last_event, o.last_event_at, o.created_at, o.updated_at FROM outbound o "
    "LEFT JOIN users u ON u.id = o.sender_user_id LEFT JOIN addresses a ON a.id = o.from_address_id ";

OutboxRow read_outbox(const db::Stmt& s) {
  OutboxRow r;
  r.id = s.i64(0);
  r.uuid = s.text(1);
  r.sender_user_id = s.i64(2);
  r.sender_email = s.text(3);
  r.from_email = s.text(4);
  r.status = s.text(5);
  r.status_detail = s.opt_text(6);
  r.error_name = s.opt_text(7);
  r.scheduled_at = s.opt_i64(8);
  r.scheduled_via = s.opt_text(9);
  r.resend_id = s.opt_text(10);
  r.total_bytes = s.i64(11);
  r.last_event = s.opt_text(12);
  r.last_event_at = s.opt_i64(13);
  r.created_at = s.i64(14);
  r.updated_at = s.i64(15);
  return r;
}

constexpr std::string_view kJobSelect =
    "SELECT id, kind, lane, priority, payload, state, run_at, attempts, max_attempts, locked_until, "
    "dedupe_key, last_error, created_at, updated_at FROM jobs ";

JobRow read_job(const db::Stmt& s) {
  JobRow r;
  r.id = s.i64(0);
  r.kind = s.text(1);
  r.lane = s.text(2);
  r.priority = s.i64(3);
  if (auto v = parse_json_text(s.text(4)); v && v->is_object()) r.payload = std::move(v->as_object());
  r.state = s.text(5);
  r.run_at = s.i64(6);
  r.attempts = s.i64(7);
  r.max_attempts = s.i64(8);
  r.locked_until = s.opt_i64(9);
  r.dedupe_key = s.opt_text(10);
  r.last_error = s.opt_text(11);
  r.created_at = s.i64(12);
  r.updated_at = s.i64(13);
  return r;
}

bool valid_job_state(std::string_view s) {
  return s == "pending" || s == "running" || s == "done" || s == "dead" || s == "canceled";
}

}  // namespace

std::vector<AdminUserRow> list_users_admin(db::Conn& c) { return load_admin_users(c, std::nullopt); }

std::optional<AdminUserRow> get_user_admin(db::Conn& c, int64_t user_id) {
  auto v = load_admin_users(c, user_id);
  if (v.empty()) return std::nullopt;
  return std::move(v.front());
}

WebhookEventPage list_webhook_events(db::Conn& c, std::optional<std::string_view> type,
                                     std::optional<std::string_view> cursor, int limit) {
  // The cursor is the id of the last row of the previous page (rows are id DESC ≈ newest first).
  std::optional<int64_t> before;
  if (cursor && !cursor->empty()) {
    before = parse_cursor_id(*cursor);
    if (!before) invalid("cursor");
  }
  std::optional<std::string> t;
  if (type && !type->empty()) t = std::string(*type);
  limit = clamp_limit(limit, 200);
  WebhookEventPage page;
  auto s = c.prepare(
      "SELECT id, svix_id, type, resend_email_id, received_at, processed_at, result "
      "FROM webhook_events WHERE (? IS NULL OR type=?) AND (? IS NULL OR id<?) "
      "ORDER BY id DESC LIMIT ?");
  s.bind_all(t, t, before, before, limit + 1);
  while (s.step()) {
    WebhookEventRow r;
    r.id = s.i64(0);
    r.svix_id = s.text(1);
    r.type = s.text(2);
    r.resend_email_id = s.opt_text(3);
    r.received_at = s.i64(4);
    r.processed_at = s.opt_i64(5);
    r.result = s.opt_text(6);
    page.items.push_back(std::move(r));
  }
  if (static_cast<int>(page.items.size()) > limit) {
    page.items.resize(static_cast<std::size_t>(limit));
    page.next_cursor = std::to_string(page.items.back().id);
  }
  return page;
}

std::vector<InboundRow> list_inbound(db::Conn& c, std::optional<std::string_view> state, int limit) {
  std::optional<std::string> st;
  if (state && !state->empty()) {
    if (!mail::parse_inbound_state(*state)) invalid("state");
    st = std::string(*state);
  }
  std::vector<InboundRow> out;
  auto s = c.prepare(
      "SELECT id, resend_id, state, source, message_id_header, from_email, subject, received_at, "
      "recipients_json, error, created_at, updated_at FROM inbound_emails "
      "WHERE (? IS NULL OR state=?) ORDER BY id DESC LIMIT ?");
  s.bind_all(st, st, clamp_limit(limit, 1000));
  while (s.step()) {
    InboundRow r;
    r.id = s.i64(0);
    r.resend_id = s.text(1);
    r.state = s.text(2);
    r.source = s.text(3);
    r.message_id_header = s.opt_text(4);
    r.from_email = s.opt_text(5);
    r.subject = s.opt_text(6);
    r.received_at = s.opt_i64(7);
    r.recipients = parse_recipients(s.opt_text(8));
    r.error = s.opt_text(9);
    r.created_at = s.i64(10);
    r.updated_at = s.i64(11);
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<OutboxRow> list_outbox(db::Conn& c, std::optional<std::string_view> status, int limit) {
  std::optional<std::string> st;
  if (status && !status->empty()) {
    if (!mail::parse_outbound_status(*status)) invalid("status");
    st = std::string(*status);
  }
  std::vector<OutboxRow> out;
  auto s = c.prepare(std::string(kOutboxSelect) +
                     "WHERE (? IS NULL OR o.status=?) ORDER BY o.id DESC LIMIT ?");
  s.bind_all(st, st, clamp_limit(limit, 1000));
  while (s.step()) out.push_back(read_outbox(s));
  return out;
}

std::optional<OutboxRow> get_outbox_row(db::Conn& c, int64_t outbound_id) {
  auto s = c.prepare(std::string(kOutboxSelect) + "WHERE o.id=?");
  s.bind_all(outbound_id);
  if (!s.step()) return std::nullopt;
  return read_outbox(s);
}

std::vector<JobRow> list_jobs(db::Conn& c, std::optional<std::string_view> state, int limit) {
  std::optional<std::string> st;
  if (state && !state->empty()) {
    if (!valid_job_state(*state)) invalid("state");
    st = std::string(*state);
  }
  std::vector<JobRow> out;
  auto s = c.prepare(std::string(kJobSelect) + "WHERE (? IS NULL OR state=?) ORDER BY id DESC LIMIT ?");
  s.bind_all(st, st, clamp_limit(limit, 1000));
  while (s.step()) out.push_back(read_job(s));
  return out;
}

std::optional<JobRow> get_job(db::Conn& c, int64_t job_id) {
  auto s = c.prepare(std::string(kJobSelect) + "WHERE id=?");
  s.bind_all(job_id);
  if (!s.step()) return std::nullopt;
  return read_job(s);
}

AdminStats admin_stats(db::Conn& c, int64_t now_ms) {
  AdminStats st;
  const int64_t since = now_ms - kDay;
  st.users = c.scalar<int64_t>("SELECT count(*) FROM users").value_or(0);
  st.messages = c.scalar<int64_t>("SELECT count(*) FROM messages").value_or(0);
  {
    auto s = c.prepare("SELECT count(*), COALESCE(SUM(size), 0) FROM blobs");
    if (s.step()) {
      st.storage.blob_count = s.i64(0);
      st.storage.blob_bytes = s.i64(1);
    }
  }
  st.storage_bytes = st.storage.blob_bytes;
  st.queue_pending =
      c.scalar<int64_t>("SELECT count(*) FROM jobs WHERE state IN ('pending','running')").value_or(0);
  st.queue_dead = c.scalar<int64_t>("SELECT count(*) FROM jobs WHERE state='dead'").value_or(0);
  st.queue_periodic = c.scalar<int64_t>(
                           "SELECT count(*) FROM jobs WHERE state IN ('pending','running') "
                           "AND dedupe_key LIKE 'periodic:%'")
                          .value_or(0);
  st.sent_24h =
      c.scalar<int64_t>("SELECT count(*) FROM outbound WHERE accepted_at>=?", since).value_or(0);
  st.received_24h = c.scalar<int64_t>(
                         "SELECT count(*) FROM inbound_emails WHERE state='delivered' "
                         "AND COALESCE(received_at, created_at)>=?",
                         since)
                        .value_or(0);
  st.failed_24h = c.scalar<int64_t>(
                       "SELECT count(*) FROM outbound WHERE status='failed' AND updated_at>=?", since)
                      .value_or(0);
  st.last_webhook_at = db::kv_get_i64(c, db::kv_keys::kLastWebhookAt);
  st.last_poll_at = db::kv_get_i64(c, db::kv_keys::kLastPollAt);
  const auto quota = db::kv_get(c, db::kv_keys::kQuotaBlocked);
  st.quota_blocked = quota.has_value() && !quota->empty();
  {
    auto s = c.prepare("SELECT value, updated_at FROM kv WHERE key=?");
    s.bind_all(db::kv_keys::kPollGapWarning);
    if (s.step() && !s.is_null(0)) {
      PollGap g{s.i64(1), s.text(0)};
      if (!trim(g.detail).empty() && g.detected_at >= now_ms - kPollGapShowMs) st.poll_gap = std::move(g);
    }
  }
  return st;
}

}  // namespace azm::repo
