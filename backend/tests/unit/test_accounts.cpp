// Owner: WP-D — repo/accounts unit tests (temp SQLite via TestServices; no network).
#include "test_support.hpp"

#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "db/kv.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "repo/accounts.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace azm;
namespace json = boost::json;

namespace {

constexpr int64_t T0 = 1'700'000'000'000;  // fixed "now" for repo calls (ms)
constexpr int64_t kHour = 3600LL * 1000;
constexpr int64_t kDay = 24 * kHour;

// Runs `f` and returns the ApiError it throws (FAIL when it does not throw one).
template <class F>
ApiError api_error(F&& f) {
  try {
    f();
  } catch (const ApiError& e) {
    return e;
  }
  FAIL("expected ApiError");
  return ApiError(0, "", "");
}

std::string field_of(const ApiError& e) {
  const auto* f = e.details.if_contains("field");
  return f && f->is_string() ? std::string(f->as_string()) : std::string();
}

#define CHECK_API_ERROR(expr, st, cd)          \
  do {                                         \
    const ApiError e_ = api_error([&] { expr; }); \
    CHECK(e_.status == (st));                  \
    CHECK(e_.code == (cd));                    \
  } while (0)

#define CHECK_INVALID_FIELD(expr, fld)             \
  do {                                             \
    const ApiError e_ = api_error([&] { expr; });  \
    CHECK(e_.status == 400u);                      \
    CHECK(e_.code == "invalid_field");             \
    CHECK(field_of(e_) == (fld));                  \
  } while (0)

repo::NewUser new_user(std::string email, bool admin = false, std::string name = "") {
  repo::NewUser u;
  u.email = std::move(email);
  u.display_name = std::move(name);
  u.password_hash = "$scrypt$test";
  u.is_admin = admin;
  return u;
}

struct Fx {
  test::TestServices ts;
  int64_t domain_id = 0;

  Fx() {
    domain_id = ts.db.write([](db::Tx& tx) { return repo::add_domain(tx, "team.example", T0).id; });
  }
  int64_t user(std::string email, bool admin = false, std::string name = "") {
    return ts.db.write(
        [&](db::Tx& tx) { return repo::create_user(tx, new_user(email, admin, name), T0).id; });
  }
  template <class F>
  auto read(F&& f) {
    return ts.db.read(std::forward<F>(f));
  }
  template <class F>
  auto write(F&& f) {
    return ts.db.write(std::forward<F>(f));
  }
};

// ---- raw mail-table fixtures (test-only; production writes go through mail::*) -------------
int64_t insert_thread(db::Tx& tx, int64_t owner) {
  tx.run("INSERT INTO threads(owner_id, created_at, updated_at) VALUES(?,?,?)", owner, T0, T0);
  return tx.last_insert_id();
}
int64_t insert_message(db::Tx& tx, int64_t owner, int64_t thread,
                       std::optional<int64_t> inbound_id = std::nullopt) {
  tx.run(
      "INSERT INTO messages(owner_id, thread_id, direction, inbound_id, date, created_at, updated_at) "
      "VALUES(?,?,?,?,?,?,?)",
      owner, thread, inbound_id ? "in" : "out", inbound_id, T0, T0, T0);
  return tx.last_insert_id();
}
std::string sha_of(char c) { return std::string(64, c); }
void insert_blob(db::Tx& tx, const std::string& sha, int64_t size, int64_t created = T0) {
  tx.run("INSERT INTO blobs(sha256, size, storage, created_at) VALUES(?,?,'local',?)", sha, size, created);
}
void insert_attachment(db::Tx& tx, int64_t owner, std::optional<int64_t> message_id,
                       const std::string& sha, int64_t size) {
  tx.run(
      "INSERT INTO attachments(owner_id, message_id, blob_sha256, filename, content_type, size, "
      "created_at) VALUES(?,?,?,'f.bin','application/octet-stream',?,?)",
      owner, message_id, sha, size, T0);
}
int64_t insert_inbound(db::Tx& tx, std::string_view resend_id, std::string_view state,
                       std::optional<std::string> raw_sha, std::optional<std::string> recipients,
                       int64_t created = T0, std::optional<int64_t> received = std::nullopt) {
  tx.run(
      "INSERT INTO inbound_emails(resend_id, state, source, raw_sha256, recipients_json, from_email, "
      "subject, received_at, created_at, updated_at) VALUES(?,?,'webhook',?,?,'x@ext.example','Hi',?,?,?)",
      resend_id, state, raw_sha, recipients, received, created, created);
  return tx.last_insert_id();
}
int64_t insert_outbound(db::Tx& tx, int64_t sender, int64_t from_addr, std::string_view status,
                        std::string_view uuid, int64_t updated = T0,
                        std::optional<int64_t> accepted = std::nullopt) {
  tx.run(
      "INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, payload_json, "
      "total_bytes, accepted_at, created_at, updated_at) VALUES(?,?,?,?,?,'{}',123,?,?,?)",
      uuid, sender, from_addr, status, T0, accepted, T0, updated);
  return tx.last_insert_id();
}

}  // namespace

// =============================================================================================
// Users
// =============================================================================================

TEST_CASE("create_user normalizes and creates address + settings rows", "[accounts][users]") {
  Fx fx;
  const auto u = fx.write([](db::Tx& tx) {
    auto nu = new_user("  Alice@Team.EXAMPLE ", true, "  Alice Li  ");
    nu.undo_send_seconds = 10;
    return repo::create_user(tx, nu, T0);
  });
  CHECK(u.email == "alice@team.example");
  CHECK(u.display_name == "Alice Li");
  CHECK(u.is_admin);
  CHECK_FALSE(u.disabled);
  CHECK(u.password_hash == "$scrypt$test");
  CHECK(u.password_changed_at == T0);
  CHECK(u.created_at == T0);
  CHECK_FALSE(u.last_login_at);
  fx.read([&](db::Conn& c) {
    auto a = repo::user_address(c, u.id);
    REQUIRE(a);
    CHECK(a->email == "alice@team.example");
    CHECK(a->kind == repo::AddressKind::User);
    CHECK(a->user_id == u.id);
    CHECK(a->domain_id == fx.domain_id);
    CHECK(a->display_name == "Alice Li");
    CHECK(repo::get_settings(c, u.id).undo_send_seconds == 10);
  });
  // Without undo_send_seconds the schema default (5) applies.
  const int64_t bob = fx.user("bob@team.example");
  CHECK(fx.read([&](db::Conn& c) { return repo::get_settings(c, bob).undo_send_seconds; }) == 5);
}

TEST_CASE("create_user validation and conflicts", "[accounts][users]") {
  Fx fx;
  fx.user("alice@team.example");
  auto create = [&](repo::NewUser nu) { fx.write([&](db::Tx& tx) { repo::create_user(tx, nu, T0); }); };

  for (const char* bad : {"", "not-an-email", "a b@team.example", "a+tag@team.example",
                          "\"quoted\"@team.example", "张三@team.example", "a@localhost", "a@-bad.example"}) {
    INFO(bad);
    CHECK_INVALID_FIELD(create(new_user(bad)), "email");
  }
  CHECK_API_ERROR(create(new_user("carol@other.example")), 422u, "unknown_domain");
  CHECK_API_ERROR(create(new_user("ALICE@team.example")), 409u, "address_exists");
  fx.write([&](db::Tx& tx) { test::seed_alias(tx, "support@team.example", {}); });
  CHECK_API_ERROR(create(new_user("support@team.example")), 409u, "address_exists");

  auto nu = new_user("dave@team.example");
  nu.undo_send_seconds = 7;
  CHECK_INVALID_FIELD(create(nu), "undo_send_seconds");
  nu = new_user("dave@team.example");
  nu.password_hash.clear();
  CHECK_INVALID_FIELD(create(nu), "password");
  CHECK_INVALID_FIELD(create(new_user("dave@team.example", false, std::string(101, 'x'))), "display_name");
  CHECK_INVALID_FIELD(create(new_user("dave@team.example", false, "bad\nname")), "display_name");
  // 100 code points of CJK (300 bytes) is fine.
  std::string cjk;
  for (int i = 0; i < 100; ++i) cjk += "张";
  CHECK_NOTHROW(create(new_user("dave@team.example", false, cjk)));
  // Nothing half-created by the failures.
  CHECK(fx.read([](db::Conn& c) { return c.scalar<int64_t>("SELECT count(*) FROM users"); }) == 2);
}

TEST_CASE("get_user / find_user_by_email", "[accounts][users]") {
  Fx fx;
  const int64_t id = fx.user("alice@team.example", false, "Alice");
  fx.read([&](db::Conn& c) {
    CHECK(repo::get_user(c, id)->email == "alice@team.example");
    CHECK_FALSE(repo::get_user(c, id + 100));
    CHECK(repo::find_user_by_email(c, " ALICE@Team.Example ")->id == id);
    CHECK_FALSE(repo::find_user_by_email(c, "alice+x@team.example"));
    CHECK_FALSE(repo::find_user_by_email(c, ""));
  });
}

TEST_CASE("update_user: display name, password, admin flags, disable", "[accounts][users]") {
  Fx fx;
  const int64_t admin = fx.user("admin@team.example", true, "Admin");
  const int64_t bob = fx.user("bob@team.example", false, "Bob");
  fx.ts.notifier.clear();

  SECTION("display_name updates the address and emits settings.changed") {
    const auto u = fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.display_name = " Bobby ";
      return repo::update_user(tx, bob, p, T0 + 5);
    });
    CHECK(u.display_name == "Bobby");
    CHECK(u.updated_at == T0 + 5);
    CHECK(fx.read([&](db::Conn& c) { return repo::user_address(c, bob)->display_name; }) == "Bobby");
    const auto ev = fx.ts.notifier.events_of("settings.changed");
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].user_id == bob);
    // Same value again: no hint.
    fx.ts.notifier.clear();
    fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.display_name = "Bobby";
      repo::update_user(tx, bob, p, T0 + 6);
    });
    CHECK(fx.ts.notifier.events_of("settings.changed").empty());
    CHECK_INVALID_FIELD(fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.display_name = std::string(101, 'y');
      repo::update_user(tx, bob, p, T0);
    }),
                        "display_name");
  }

  SECTION("password hash bumps password_changed_at but keeps sessions") {
    fx.write([&](db::Tx& tx) { test::seed_session(tx, bob); });
    const auto u = fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.password_hash = "$scrypt$new";
      return repo::update_user(tx, bob, p, T0 + 9);
    });
    CHECK(u.password_hash == "$scrypt$new");
    CHECK(u.password_changed_at == T0 + 9);
    CHECK(fx.read([&](db::Conn& c) {
            return c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", bob);
          }) == 1);
  }

  SECTION("disable deletes all sessions; enable again") {
    fx.write([&](db::Tx& tx) {
      test::seed_session(tx, bob);
      test::seed_session(tx, bob);
      test::seed_session(tx, admin);
    });
    auto u = fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.disabled = true;
      return repo::update_user(tx, bob, p, T0);
    });
    CHECK(u.disabled);
    fx.read([&](db::Conn& c) {
      CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", bob) == 0);
      CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", admin) == 1);
    });
    u = fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.disabled = false;
      return repo::update_user(tx, bob, p, T0);
    });
    CHECK_FALSE(u.disabled);
  }

  SECTION("last active admin cannot be demoted or disabled") {
    auto demote = [&](int64_t id) {
      fx.write([&](db::Tx& tx) {
        repo::UserPatch p;
        p.is_admin = false;
        repo::update_user(tx, id, p, T0);
      });
    };
    auto disable = [&](int64_t id) {
      fx.write([&](db::Tx& tx) {
        repo::UserPatch p;
        p.disabled = true;
        repo::update_user(tx, id, p, T0);
      });
    };
    CHECK_API_ERROR(demote(admin), 409u, "last_admin");
    CHECK_API_ERROR(disable(admin), 409u, "last_admin");
    CHECK(fx.read([](db::Conn& c) { return repo::count_active_admins(c); }) == 1);
    // A second admin makes it possible; a disabled admin does not count.
    fx.write([&](db::Tx& tx) {
      repo::UserPatch p;
      p.is_admin = true;
      repo::update_user(tx, bob, p, T0);
    });
    CHECK(fx.ts.notifier.events_of("settings.changed").size() == 1);  // Me.is_admin changed
    CHECK(fx.read([](db::Conn& c) { return repo::count_active_admins(c); }) == 2);
    disable(bob);
    CHECK(fx.read([](db::Conn& c) { return repo::count_active_admins(c); }) == 1);
    CHECK_API_ERROR(demote(admin), 409u, "last_admin");
    // Demoting a disabled admin is fine (it is not an active admin).
    CHECK_NOTHROW(demote(bob));
  }

  SECTION("missing user") {
    CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::update_user(tx, 999, {}, T0); }), 404u, "not_found");
  }
}

TEST_CASE("delete_user: self, last admin, cascade", "[accounts][users]") {
  Fx fx;
  const int64_t admin = fx.user("admin@team.example", true);
  const int64_t bob = fx.user("bob@team.example");
  auto del = [&](int64_t id, int64_t actor) { fx.write([&](db::Tx& tx) { repo::delete_user(tx, id, actor); }); };

  CHECK_API_ERROR(del(admin, admin), 409u, "cannot_delete_self");
  CHECK_API_ERROR(del(999, admin), 404u, "not_found");
  CHECK_API_ERROR(del(admin, bob), 409u, "last_admin");

  fx.write([&](db::Tx& tx) {
    test::seed_session(tx, bob);
    repo::create_label(tx, bob, {"Work", "#112233", std::nullopt}, T0);
    const int64_t th = insert_thread(tx, bob);
    insert_message(tx, bob, th);
    const int64_t sales = test::seed_alias(tx, "sales@team.example", {{bob, true}});
    // Outbound rows sent by bob (from his mailbox and from the alias) cascade with him.
    insert_outbound(tx, bob, test::address_id(tx.conn(), "bob@team.example"), "sent", "ob-1");
    insert_outbound(tx, bob, sales, "sent", "ob-2");
  });
  del(bob, admin);
  fx.read([&](db::Conn& c) {
    CHECK_FALSE(repo::get_user(c, bob));
    CHECK_FALSE(repo::find_address(c, "bob@team.example"));
    for (const char* t : {"sessions", "user_settings", "alias_members"})
      CHECK(c.scalar<int64_t>(std::string("SELECT count(*) FROM ") + t + " WHERE user_id=?", bob) == 0);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM labels WHERE owner_id=?", bob) == 0);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM messages WHERE owner_id=?", bob) == 0);
    CHECK(repo::find_address(c, "sales@team.example"));  // the alias itself stays
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM outbound") == 0);
  });
}

TEST_CASE("record_login and count_active_admins", "[accounts][users]") {
  Fx fx;
  const int64_t a = fx.user("a@team.example", true);
  fx.user("b@team.example", true);
  fx.user("c@team.example");
  fx.write([&](db::Tx& tx) { repo::record_login(tx, a, T0 + 42); });
  fx.read([&](db::Conn& c) {
    CHECK(repo::get_user(c, a)->last_login_at == T0 + 42);
    CHECK(repo::count_active_admins(c) == 2);
  });
}

// =============================================================================================
// Sessions
// =============================================================================================

TEST_CASE("sessions: create, find, expiry, touch", "[accounts][sessions]") {
  Fx fx;
  const int64_t uid = fx.user("alice@team.example");
  const int64_t ttl = 30 * kDay;
  const std::string long_ua(400, 'u');
  const auto cs = fx.write(
      [&](db::Tx& tx) { return repo::create_session(tx, uid, ttl, long_ua, "203.0.113.9", T0); });

  CHECK(cs.token.size() == 43);  // 32 random bytes, b64url without padding
  CHECK(crypto::b64url_decode(cs.token)->size() == 32);
  CHECK(cs.session.id > 0);
  CHECK(cs.session.user_id == uid);
  CHECK(cs.session.expires_at == T0 + ttl);
  CHECK(cs.session.user_agent.size() == 256);
  CHECK(cs.session.ip == "203.0.113.9");
  fx.read([&](db::Conn& c) {
    // Only the hash is stored, as a BLOB.
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE token_hash=?",
                            crypto::to_bytes(crypto::sha256(cs.token))) == 1);
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE token_hash=?", cs.token) == 0);

    auto a = repo::find_session(c, cs.token, T0 + 1000);
    REQUIRE(a);
    CHECK(a->session.id == cs.session.id);
    CHECK(a->user.id == uid);
    CHECK(a->user.email == "alice@team.example");
    CHECK_FALSE(a->needs_touch);
    CHECK(repo::find_session(c, cs.token, T0 + kHour)->needs_touch);
    CHECK(repo::find_session(c, cs.token, T0 + 10, 5)->needs_touch);
    // Expired exactly at expires_at.
    CHECK(repo::find_session(c, cs.token, T0 + ttl - 1));
    CHECK_FALSE(repo::find_session(c, cs.token, T0 + ttl));
    CHECK_FALSE(repo::find_session(c, cs.token + "x", T0));
    CHECK_FALSE(repo::find_session(c, "", T0));
    CHECK_FALSE(repo::find_session(c, std::string(600, 'a'), T0));
  });

  // Sliding expiry.
  CHECK(fx.write([&](db::Tx& tx) { return repo::touch_session(tx, cs.session.id, T0 + 2 * kHour, ttl); }));
  fx.read([&](db::Conn& c) {
    auto a = repo::find_session(c, cs.token, T0 + 2 * kHour + 1);
    REQUIRE(a);
    CHECK(a->session.last_seen_at == T0 + 2 * kHour);
    CHECK(a->session.expires_at == T0 + 2 * kHour + ttl);
    CHECK(repo::find_session(c, cs.token, T0 + ttl + kHour));  // would have expired without touch
  });
  // An expired session is never resurrected by a touch.
  CHECK_FALSE(fx.write([&](db::Tx& tx) { return repo::touch_session(tx, cs.session.id, T0 + 10 * ttl, ttl); }));
  CHECK_FALSE(fx.write([&](db::Tx& tx) { return repo::touch_session(tx, 9999, T0, ttl); }));

  // Disabled users have no valid sessions (row kept until disable/cleanup).
  fx.write([&](db::Tx& tx) { tx.run("UPDATE users SET disabled=1 WHERE id=?", uid); });
  CHECK_FALSE(fx.read([&](db::Conn& c) { return repo::find_session(c, cs.token, T0 + 1); }));

  CHECK_THROWS_AS(fx.write([&](db::Tx& tx) { repo::create_session(tx, uid, 0, "", "", T0); }),
                  std::invalid_argument);
}

TEST_CASE("sessions: revoke, revoke_all, purge", "[accounts][sessions]") {
  Fx fx;
  const int64_t a = fx.user("alice@team.example");
  const int64_t b = fx.user("bob@team.example");
  std::vector<int64_t> ids;
  fx.write([&](db::Tx& tx) {
    for (int i = 0; i < 3; ++i) ids.push_back(repo::create_session(tx, a, kDay, "", "", T0).session.id);
    repo::create_session(tx, b, kDay, "", "", T0);
    repo::create_session(tx, b, kHour, "", "", T0);  // expires first
  });
  CHECK(fx.write([&](db::Tx& tx) { return repo::revoke_session(tx, ids[0]); }));
  CHECK_FALSE(fx.write([&](db::Tx& tx) { return repo::revoke_session(tx, ids[0]); }));

  const auto revoked = fx.write([&](db::Tx& tx) { return repo::revoke_all_sessions(tx, a, ids[2]); });
  CHECK(revoked == std::vector<int64_t>{ids[1]});
  fx.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM sessions WHERE user_id=?", a) == 1);
    CHECK(c.scalar<int64_t>("SELECT id FROM sessions WHERE user_id=?", a) == ids[2]);
  });
  CHECK(fx.write([&](db::Tx& tx) { return repo::revoke_all_sessions(tx, a); }) ==
        std::vector<int64_t>{ids[2]});
  CHECK(fx.write([&](db::Tx& tx) { return repo::revoke_all_sessions(tx, a); }).empty());

  CHECK(fx.write([&](db::Tx& tx) { return repo::purge_expired_sessions(tx, T0 + kHour); }) == 1);
  CHECK(fx.write([&](db::Tx& tx) { return repo::purge_expired_sessions(tx, T0 + kHour); }) == 0);
  CHECK(fx.read([&](db::Conn& c) { return c.scalar<int64_t>("SELECT count(*) FROM sessions"); }) == 1);
}

TEST_CASE("session_token_hash is raw sha256", "[accounts][sessions]") {
  CHECK(repo::session_token_hash("tok") == crypto::sha256("tok"));
  CHECK(repo::session_token_hash("tok").size() == 32);
}

// =============================================================================================
// Addresses, aliases, identities
// =============================================================================================

TEST_CASE("aliases: create, get, list", "[accounts][aliases]") {
  Fx fx;
  const int64_t zed = fx.user("zed@team.example", false, "Zed");
  const int64_t amy = fx.user("amy@team.example", false, "Amy");
  fx.ts.notifier.clear();
  const auto a = fx.write([&](db::Tx& tx) {
    repo::AliasInput in;
    in.email = " Support@Team.example";
    in.display_name = "客服";
    in.share_sent = false;
    in.members = {{zed, true}, {amy, false}};
    return repo::create_alias(tx, in, T0);
  });
  CHECK(a.email == "support@team.example");
  CHECK(a.display_name == "客服");
  CHECK_FALSE(a.share_sent);
  CHECK(a.created_at == T0);
  REQUIRE(a.members.size() == 2);
  CHECK(a.members[0].email == "amy@team.example");  // by user email
  CHECK(a.members[0].display_name == "Amy");
  CHECK_FALSE(a.members[0].can_send_as);
  CHECK(a.members[1].user_id == zed);
  CHECK(a.members[1].can_send_as);
  CHECK(fx.ts.notifier.events_of("settings.changed").size() == 2);  // identities of both members

  fx.write([&](db::Tx& tx) {
    repo::AliasInput in;
    in.email = "billing@team.example";
    return repo::create_alias(tx, in, T0);
  });
  fx.read([&](db::Conn& c) {
    const auto all = repo::list_aliases(c);
    REQUIRE(all.size() == 2);
    CHECK(all[0].email == "billing@team.example");
    CHECK(all[0].members.empty());
    CHECK(all[0].share_sent);  // default
    CHECK(all[1].id == a.id);
    CHECK(repo::get_alias(c, a.id)->members.size() == 2);
    // A user's mailbox address is not an alias.
    CHECK_FALSE(repo::get_alias(c, repo::user_address(c, zed)->id));
    CHECK_FALSE(repo::get_alias(c, 12345));
    auto row = repo::find_address(c, "SUPPORT@team.example");
    REQUIRE(row);
    CHECK(row->kind == repo::AddressKind::Alias);
    CHECK_FALSE(row->user_id);
    CHECK(repo::get_address(c, a.id)->email == "support@team.example");
  });
}

TEST_CASE("aliases: create errors", "[accounts][aliases]") {
  Fx fx;
  const int64_t u = fx.user("u@team.example");
  auto create = [&](std::string email, std::vector<repo::AliasMemberInput> members = {}) {
    fx.write([&](db::Tx& tx) {
      repo::AliasInput in;
      in.email = email;
      in.members = members;
      repo::create_alias(tx, in, T0);
    });
  };
  CHECK_INVALID_FIELD(create("nope"), "email");
  CHECK_API_ERROR(create("x@elsewhere.example"), 422u, "unknown_domain");
  CHECK_API_ERROR(create("U@team.example"), 409u, "address_exists");
  CHECK_INVALID_FIELD(create("a@team.example", {{u, true}, {u, false}}), "members");
  CHECK_INVALID_FIELD(create("a@team.example", {{777, true}}), "members");
  CHECK_INVALID_FIELD(create("a@team.example", {{0, true}}), "members");
  create("a@team.example");
  CHECK_API_ERROR(create("a@team.example"), 409u, "address_exists");
  // The user namespace is shared: a user cannot take an alias address either.
  CHECK_API_ERROR(fx.write([](db::Tx& tx) { repo::create_user(tx, new_user("a@team.example"), T0); }),
                  409u, "address_exists");
}

TEST_CASE("aliases: update replaces members wholesale", "[accounts][aliases]") {
  Fx fx;
  const int64_t u1 = fx.user("u1@team.example");
  const int64_t u2 = fx.user("u2@team.example");
  const int64_t u3 = fx.user("u3@team.example");
  const int64_t alias = fx.write([&](db::Tx& tx) {
    return test::seed_alias(tx, "sales@team.example", {{u1, true}, {u2, false}});
  });
  fx.write([&](db::Tx& tx) { test::seed_alias(tx, "other@team.example", {}); });
  fx.ts.notifier.clear();

  const auto a = fx.write([&](db::Tx& tx) {
    repo::AliasPatch p;
    p.email = "Deals@team.example";
    p.display_name = "Deals";
    p.share_sent = false;
    p.members = std::vector<repo::AliasMemberInput>{{u3, true}};
    return repo::update_alias(tx, alias, p);
  });
  CHECK(a.email == "deals@team.example");
  CHECK(a.display_name == "Deals");
  CHECK_FALSE(a.share_sent);
  REQUIRE(a.members.size() == 1);
  CHECK(a.members[0].user_id == u3);
  CHECK(fx.ts.notifier.events_of("settings.changed").size() == 3);  // old ∪ new members

  // Patch without members keeps them; renaming to itself is fine.
  const auto b = fx.write([&](db::Tx& tx) {
    repo::AliasPatch p;
    p.email = "deals@team.example";
    return repo::update_alias(tx, alias, p);
  });
  CHECK(b.members.size() == 1);
  // Empty member list clears them.
  CHECK(fx.write([&](db::Tx& tx) {
          repo::AliasPatch p;
          p.members = std::vector<repo::AliasMemberInput>{};
          return repo::update_alias(tx, alias, p);
        }).members.empty());

  auto patch_email = [&](std::string e) {
    fx.write([&](db::Tx& tx) {
      repo::AliasPatch p;
      p.email = e;
      repo::update_alias(tx, alias, p);
    });
  };
  CHECK_API_ERROR(patch_email("other@team.example"), 409u, "address_exists");
  CHECK_API_ERROR(patch_email("u1@team.example"), 409u, "address_exists");
  CHECK_API_ERROR(patch_email("x@nowhere.example"), 422u, "unknown_domain");
  CHECK_INVALID_FIELD(patch_email("bad"), "email");
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::update_alias(tx, 4242, {}); }), 404u, "not_found");
  const int64_t user_addr = fx.read([&](db::Conn& c) { return repo::user_address(c, u1)->id; });
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::update_alias(tx, user_addr, {}); }), 404u, "not_found");
}

TEST_CASE("aliases: delete and alias_in_use", "[accounts][aliases]") {
  Fx fx;
  const int64_t u = fx.user("u@team.example");
  const auto [used, unused] = fx.write([&](db::Tx& tx) {
    return std::pair{test::seed_alias(tx, "used@team.example", {{u, true}}),
                     test::seed_alias(tx, "unused@team.example", {{u, false}})};
  });
  int64_t draft = 0;
  fx.write([&](db::Tx& tx) {
    insert_outbound(tx, u, used, "sent", "uuid-1");
    draft = insert_message(tx, u, insert_thread(tx, u));
    tx.run("UPDATE messages SET from_address_id=? WHERE id=?", unused, draft);
  });
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::delete_alias(tx, used); }), 409u, "alias_in_use");
  fx.ts.notifier.clear();
  fx.write([&](db::Tx& tx) { repo::delete_alias(tx, unused); });
  CHECK(fx.ts.notifier.events_of("settings.changed").size() == 1);
  fx.read([&](db::Conn& c) {
    CHECK_FALSE(repo::get_alias(c, unused));
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM alias_members WHERE alias_id=?", unused) == 0);
    // Drafts that used the alias fall back to NULL (ON DELETE SET NULL); the message survives.
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM messages WHERE id=? AND from_address_id IS NULL", draft) == 1);
  });
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::delete_alias(tx, unused); }), 404u, "not_found");
  const int64_t user_addr = fx.read([&](db::Conn& c) { return repo::user_address(c, u)->id; });
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::delete_alias(tx, user_addr); }), 404u, "not_found");
}

TEST_CASE("identities_for_user and can_send_as share the send-as rule", "[accounts][aliases]") {
  Fx fx;
  const int64_t alice = fx.user("alice@team.example", false, "Alice");
  const int64_t bob = fx.user("bob@team.example", false, "Bob");
  const auto [zsupport, asales, nosend] = fx.write([&](db::Tx& tx) {
    return std::tuple{test::seed_alias(tx, "zsupport@team.example", {{alice, true}, {bob, false}}, true, "Support"),
                      test::seed_alias(tx, "asales@team.example", {{alice, true}}, true, "Sales"),
                      test::seed_alias(tx, "member-only@team.example", {{alice, false}})};
  });
  fx.read([&](db::Conn& c) {
    const auto ids = repo::identities_for_user(c, alice);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0].email == "alice@team.example");
    CHECK(ids[0].display_name == "Alice");
    CHECK(ids[0].kind == repo::AddressKind::User);
    CHECK(ids[0].is_default);
    CHECK(ids[1].email == "asales@team.example");
    CHECK(ids[1].address_id == asales);
    CHECK(ids[1].display_name == "Sales");
    CHECK(ids[1].kind == repo::AddressKind::Alias);
    CHECK_FALSE(ids[1].is_default);
    CHECK(ids[2].address_id == zsupport);

    const auto bob_ids = repo::identities_for_user(c, bob);
    REQUIRE(bob_ids.size() == 1);
    CHECK(bob_ids[0].email == "bob@team.example");
    CHECK(repo::identities_for_user(c, 999).empty());

    const int64_t alice_addr = repo::user_address(c, alice)->id;
    const int64_t bob_addr = repo::user_address(c, bob)->id;
    CHECK(repo::can_send_as(c, alice, alice_addr));
    CHECK(repo::can_send_as(c, alice, zsupport));
    CHECK_FALSE(repo::can_send_as(c, alice, nosend));
    CHECK_FALSE(repo::can_send_as(c, alice, bob_addr));
    CHECK_FALSE(repo::can_send_as(c, bob, zsupport));
    CHECK_FALSE(repo::can_send_as(c, bob, 99999));
  });
}

// =============================================================================================
// Domains
// =============================================================================================

TEST_CASE("domains: add, find, remove", "[accounts][domains]") {
  Fx fx;
  const auto d = fx.write([](db::Tx& tx) { return repo::add_domain(tx, "  Mail.Example.COM. ", T0); });
  CHECK(d.name == "mail.example.com");
  CHECK(d.receiving_enabled);
  CHECK(d.created_at == T0);
  CHECK_API_ERROR(fx.write([](db::Tx& tx) { repo::add_domain(tx, "MAIL.example.com", T0); }), 409u,
                  "domain_exists");
  for (const char* bad : {"", "localhost", "-a.example", "a-.example", "a..example", "1.2.3.4",
                          "例子.example", "a_b.example", ".example"}) {
    INFO(bad);
    CHECK_INVALID_FIELD(fx.write([&](db::Tx& tx) { repo::add_domain(tx, bad, T0); }), "name");
  }
  CHECK_INVALID_FIELD(fx.write([](db::Tx& tx) { repo::add_domain(tx, std::string(64, 'a') + ".example", T0); }),
                      "name");
  CHECK_NOTHROW(fx.write([](db::Tx& tx) { repo::add_domain(tx, "xn--fiqs8s.example", T0); }));

  fx.read([&](db::Conn& c) {
    const auto all = repo::list_domains(c);
    REQUIRE(all.size() == 3);
    CHECK(all[0].name == "mail.example.com");
    CHECK(all[1].name == "team.example");
    CHECK(all[2].name == "xn--fiqs8s.example");
    CHECK(repo::get_domain(c, d.id)->name == "mail.example.com");
    CHECK(repo::find_domain(c, "MAIL.example.com.")->id == d.id);
    CHECK_FALSE(repo::find_domain(c, "nope.example"));
    CHECK_FALSE(repo::find_domain(c, ""));
    CHECK(repo::is_local_domain(c, "Team.Example"));
    CHECK_FALSE(repo::is_local_domain(c, "gmail.com"));
  });

  fx.user("someone@team.example");
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::remove_domain(tx, fx.domain_id); }), 409u, "domain_in_use");
  fx.write([&](db::Tx& tx) { repo::remove_domain(tx, d.id); });
  CHECK_FALSE(fx.read([&](db::Conn& c) { return repo::get_domain(c, d.id); }));
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::remove_domain(tx, d.id); }), 404u, "not_found");
}

// =============================================================================================
// Settings
// =============================================================================================

TEST_CASE("settings: defaults and partial update", "[accounts][settings]") {
  Fx fx;
  const int64_t uid = fx.user("alice@team.example", false, "Alice");

  // Row missing → defaults (display_name still from users).
  fx.write([&](db::Tx& tx) { tx.run("DELETE FROM user_settings WHERE user_id=?", uid); });
  auto s = fx.read([&](db::Conn& c) { return repo::get_settings(c, uid); });
  CHECK(s.undo_send_seconds == 5);
  CHECK(s.signature_html.empty());
  CHECK(s.signature_enabled);
  CHECK(s.timezone == "Asia/Shanghai");
  CHECK(s.page_size == 50);
  CHECK(s.remote_images == repo::RemoteImages::Ask);
  CHECK(s.trusted_image_senders.empty());
  CHECK(s.display_name == "Alice");

  fx.ts.notifier.clear();
  s = fx.write([&](db::Tx& tx) {
    repo::SettingsPatch p;
    p.undo_send_seconds = 30;
    p.signature_html = "<p>— Alice</p>";
    p.remote_images = repo::RemoteImages::Always;
    p.trusted_image_senders = std::vector<std::string>{"Z@x.example", "a@y.example", "z@x.example "};
    return repo::update_settings(tx, uid, p, T0);
  });
  CHECK(s.undo_send_seconds == 30);
  CHECK(s.signature_html == "<p>— Alice</p>");
  CHECK(s.signature_enabled);  // untouched
  CHECK(s.timezone == "Asia/Shanghai");
  CHECK(s.remote_images == repo::RemoteImages::Always);
  CHECK(s.trusted_image_senders == std::vector<std::string>{"a@y.example", "z@x.example"});
  CHECK(fx.ts.notifier.events_of("settings.changed").size() == 1);

  s = fx.write([&](db::Tx& tx) {
    repo::SettingsPatch p;
    p.signature_enabled = false;
    p.timezone = "America/New_York";
    p.page_size = 100;
    p.display_name = " 李雷 ";
    p.trusted_image_senders = std::vector<std::string>{};
    return repo::update_settings(tx, uid, p, T0);
  });
  CHECK(s.undo_send_seconds == 30);  // kept from before
  CHECK_FALSE(s.signature_enabled);
  CHECK(s.timezone == "America/New_York");
  CHECK(s.page_size == 100);
  CHECK(s.display_name == "李雷");
  CHECK(s.trusted_image_senders.empty());
  fx.read([&](db::Conn& c) {
    CHECK(repo::get_user(c, uid)->display_name == "李雷");
    CHECK(repo::user_address(c, uid)->display_name == "李雷");
    CHECK(repo::identities_for_user(c, uid)[0].display_name == "李雷");
  });
  // An empty patch is valid and returns the current settings.
  CHECK(fx.write([&](db::Tx& tx) { return repo::update_settings(tx, uid, {}, T0); }).page_size == 100);
}

TEST_CASE("settings: validation", "[accounts][settings]") {
  Fx fx;
  const int64_t uid = fx.user("alice@team.example", false, "Alice");
  auto upd = [&](repo::SettingsPatch p) { fx.write([&](db::Tx& tx) { repo::update_settings(tx, uid, p, T0); }); };
  repo::SettingsPatch p;

  p = {};
  p.undo_send_seconds = 7;
  CHECK_INVALID_FIELD(upd(p), "undo_send_seconds");
  for (int ok : {0, 5, 10, 20, 30}) {
    p = {};
    p.undo_send_seconds = ok;
    CHECK_NOTHROW(upd(p));
  }
  for (int bad : {9, 101}) {
    p = {};
    p.page_size = bad;
    CHECK_INVALID_FIELD(upd(p), "page_size");
  }
  for (const char* bad : {"", "Asia/Shang hai", "../etc/passwd", "Europe/Zürich"}) {
    p = {};
    p.timezone = bad;
    CHECK_INVALID_FIELD(upd(p), "timezone");
  }
  p = {};
  p.timezone = std::string(65, 'a');
  CHECK_INVALID_FIELD(upd(p), "timezone");
  p = {};
  p.timezone = "Etc/GMT+8";
  CHECK_NOTHROW(upd(p));
  p = {};
  p.signature_html = std::string((64u << 10) + 1, 'x');
  CHECK_INVALID_FIELD(upd(p), "signature_html");
  p = {};
  p.signature_html = std::string("\xff\xfe");
  CHECK_INVALID_FIELD(upd(p), "signature_html");
  p = {};
  p.display_name = "   ";
  CHECK_INVALID_FIELD(upd(p), "display_name");
  p = {};
  p.display_name = std::string(101, 'n');
  CHECK_INVALID_FIELD(upd(p), "display_name");
  {
    // A user without a name may PUT back the Settings it fetched (display_name "").
    const int64_t nameless = fx.user("nameless@team.example");
    repo::SettingsPatch round_trip;
    round_trip.display_name = "";
    round_trip.page_size = 20;
    const auto s = fx.write([&](db::Tx& tx) { return repo::update_settings(tx, nameless, round_trip, T0); });
    CHECK(s.display_name.empty());
    CHECK(s.page_size == 20);
  }
  p = {};
  p.trusted_image_senders = std::vector<std::string>{"ok@x.example", "not-an-email"};
  CHECK_INVALID_FIELD(upd(p), "trusted_image_senders");
  p = {};
  p.trusted_image_senders = std::vector<std::string>(501, "a@b.example");
  CHECK_INVALID_FIELD(upd(p), "trusted_image_senders");

  // A failed validation writes nothing (the timezone above was the last success).
  CHECK(fx.read([&](db::Conn& c) { return repo::get_settings(c, uid).timezone; }) == "Etc/GMT+8");
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::update_settings(tx, 999, {}, T0); }), 404u, "not_found");
}

// =============================================================================================
// Labels
// =============================================================================================

TEST_CASE("labels: create, order, validation, uniqueness", "[accounts][labels]") {
  Fx fx;
  const int64_t a = fx.user("a@team.example");
  const int64_t b = fx.user("b@team.example");
  fx.ts.notifier.clear();
  auto create = [&](int64_t owner, std::string name, std::string color = "#AABBCC",
                    std::optional<int64_t> order = std::nullopt) {
    return fx.write([&](db::Tx& tx) { return repo::create_label(tx, owner, {name, color, order}, T0); });
  };
  const auto l1 = create(a, "  工作 ");
  CHECK(l1.name == "工作");
  CHECK(l1.color == "#aabbcc");
  CHECK(l1.sort_order == 0);
  CHECK(l1.owner_id == a);
  CHECK(l1.created_at == T0);
  CHECK(create(a, "Travel", "").color == "#9aa0a6");  // empty → default
  CHECK(create(a, "Later", "#000000").sort_order == 2);
  CHECK(create(a, "First", "#000000", -5).sort_order == -5);
  CHECK(fx.ts.notifier.events_of("labels.changed").size() == 4);
  CHECK(fx.ts.notifier.events_of("labels.changed")[0].user_id == a);

  CHECK_API_ERROR(create(a, "travel"), 409u, "label_exists");
  CHECK_NOTHROW(create(b, "Travel"));  // per owner
  CHECK_INVALID_FIELD(create(a, "   "), "name");
  CHECK_INVALID_FIELD(create(a, std::string(65, 'n')), "name");
  CHECK_INVALID_FIELD(create(a, "tab\there"), "name");
  for (const char* bad : {"red", "#abc", "#gggggg", "aabbcc", "#aabbccdd"}) {
    INFO(bad);
    CHECK_INVALID_FIELD(create(a, "X", bad), "color");
  }
  CHECK_INVALID_FIELD(create(a, "Y", "#000000", int64_t{5'000'000'000}), "sort_order");

  fx.read([&](db::Conn& c) {
    const auto all = repo::list_labels(c, a);
    REQUIRE(all.size() == 4);
    CHECK(all[0].name == "First");
    CHECK(all[1].name == "工作");
    CHECK(all[2].name == "Travel");
    CHECK(all[3].name == "Later");
    CHECK(repo::list_labels(c, b).size() == 1);
    CHECK(repo::get_label(c, a, l1.id)->name == "工作");
    CHECK_FALSE(repo::get_label(c, b, l1.id));  // IDOR: foreign label = missing
  });
}

TEST_CASE("labels: update", "[accounts][labels]") {
  Fx fx;
  const int64_t a = fx.user("a@team.example");
  const int64_t b = fx.user("b@team.example");
  const auto [x, y] = fx.write([&](db::Tx& tx) {
    return std::pair{repo::create_label(tx, a, {"X", "#111111", std::nullopt}, T0).id,
                     repo::create_label(tx, a, {"Y", "#222222", std::nullopt}, T0).id};
  });
  fx.ts.notifier.clear();
  auto upd = [&](int64_t owner, int64_t id, repo::LabelPatch p) {
    return fx.write([&](db::Tx& tx) { return repo::update_label(tx, owner, id, p); });
  };
  repo::LabelPatch p;
  p.name = "Renamed";
  p.color = "#ABCDEF";
  p.sort_order = 9;
  const auto l = upd(a, x, p);
  CHECK(l.name == "Renamed");
  CHECK(l.color == "#abcdef");
  CHECK(l.sort_order == 9);
  CHECK(fx.ts.notifier.events_of("labels.changed").size() == 1);

  p = {};
  p.name = "renamed";  // same label, different case → allowed
  CHECK(upd(a, x, p).name == "renamed");
  p = {};
  p.name = "y";
  CHECK_API_ERROR(upd(a, x, p), 409u, "label_exists");
  p = {};
  p.color = "blue";
  CHECK_INVALID_FIELD(upd(a, x, p), "color");
  p = {};
  p.name = "";
  CHECK_INVALID_FIELD(upd(a, x, p), "name");
  CHECK_API_ERROR(upd(b, y, {}), 404u, "not_found");
  CHECK(upd(a, y, {}).name == "Y");  // empty patch
}

TEST_CASE("labels: delete emits labels.changed and threads.changed", "[accounts][labels]") {
  Fx fx;
  const int64_t a = fx.user("a@team.example");
  const int64_t b = fx.user("b@team.example");
  int64_t label = 0, t1 = 0, t2 = 0;
  fx.write([&](db::Tx& tx) {
    label = repo::create_label(tx, a, {"L", "#111111", std::nullopt}, T0).id;
    t1 = insert_thread(tx, a);
    t2 = insert_thread(tx, a);
    for (int64_t m : {insert_message(tx, a, t1), insert_message(tx, a, t1), insert_message(tx, a, t2)})
      tx.run("INSERT INTO message_labels(message_id, label_id) VALUES(?,?)", m, label);
    insert_message(tx, a, insert_thread(tx, a));  // unlabeled thread
  });
  CHECK_API_ERROR(fx.write([&](db::Tx& tx) { repo::delete_label(tx, b, label); }), 404u, "not_found");
  fx.ts.notifier.clear();
  fx.write([&](db::Tx& tx) { repo::delete_label(tx, a, label); });
  CHECK(fx.ts.notifier.events_of("labels.changed").size() == 1);
  const auto tc = fx.ts.notifier.events_of("threads.changed");
  REQUIRE(tc.size() == 1);
  CHECK(tc[0].user_id == a);
  CHECK(json::value(tc[0].data) == json::parse("{\"thread_ids\":[" + std::to_string(t1) + "," +
                                               std::to_string(t2) + "]}"));
  fx.read([&](db::Conn& c) {
    CHECK_FALSE(repo::get_label(c, a, label));
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM message_labels") == 0);
  });
  // A label on no message only emits labels.changed.
  const int64_t l2 = fx.write([&](db::Tx& tx) { return repo::create_label(tx, a, {"M", "", std::nullopt}, T0).id; });
  fx.ts.notifier.clear();
  fx.write([&](db::Tx& tx) { repo::delete_label(tx, a, l2); });
  CHECK(fx.ts.notifier.events_of("threads.changed").empty());
}

// =============================================================================================
// Audit
// =============================================================================================

TEST_CASE("audit rows", "[accounts][audit]") {
  Fx fx;
  const int64_t a = fx.user("a@team.example");
  fx.write([&](db::Tx& tx) {
    repo::audit(tx, a, "user.create", "b@team.example", {{"is_admin", false}}, "198.51.100.1", T0);
    repo::audit(tx, std::nullopt, "login.failure", "", {}, "", T0 + 1);
  });
  fx.read([&](db::Conn& c) {
    auto s = c.prepare("SELECT actor_user_id, action, target, detail_json, ip, at FROM audit_log ORDER BY id");
    REQUIRE(s.step());
    CHECK(s.opt_i64(0) == a);
    CHECK(s.text(1) == "user.create");
    CHECK(s.text(2) == "b@team.example");
    CHECK(json::parse(s.text(3)) == json::parse(R"({"is_admin":false})"));
    CHECK(s.text(4) == "198.51.100.1");
    CHECK(s.i64(5) == T0);
    REQUIRE(s.step());
    CHECK(s.is_null(0));
    CHECK(s.is_null(2));
    CHECK(s.is_null(3));
    CHECK(s.is_null(4));
  });
}

// =============================================================================================
// Admin read models
// =============================================================================================

TEST_CASE("admin users: message_count, storage_bytes, aliases", "[accounts][admin]") {
  Fx fx;
  const int64_t zed = fx.user("zed@team.example", true, "Zed");
  const int64_t amy = fx.user("amy@team.example");
  fx.write([&](db::Tx& tx) {
    test::seed_alias(tx, "b-alias@team.example", {{amy, true}});
    test::seed_alias(tx, "a-alias@team.example", {{amy, false}});
    insert_blob(tx, sha_of('a'), 1000);
    insert_blob(tx, sha_of('b'), 50);
    insert_blob(tx, sha_of('c'), 7);
    const int64_t in1 = insert_inbound(tx, "re_1", "delivered", sha_of('a'), std::nullopt);
    const int64_t th = insert_thread(tx, amy);
    const int64_t m1 = insert_message(tx, amy, th, in1);
    insert_message(tx, amy, th);
    insert_attachment(tx, amy, m1, sha_of('b'), 50);
    insert_attachment(tx, amy, std::nullopt, sha_of('c'), 7);  // unattached upload counts too
    // The same inbound mail for zed (raw counted per recipient).
    insert_message(tx, zed, insert_thread(tx, zed), in1);
    repo::record_login(tx, zed, T0 + 1);
  });
  fx.read([&](db::Conn& c) {
    const auto rows = repo::list_users_admin(c);
    REQUIRE(rows.size() == 2);
    const auto& amy_row = rows[0];
    CHECK(amy_row.user.email == "amy@team.example");
    CHECK(amy_row.message_count == 2);
    CHECK(amy_row.storage_bytes == 1000 + 50 + 7);
    REQUIRE(amy_row.aliases.size() == 2);
    CHECK(amy_row.aliases[0].email == "a-alias@team.example");
    CHECK_FALSE(amy_row.aliases[0].can_send_as);
    CHECK(amy_row.aliases[1].can_send_as);
    const auto& zed_row = rows[1];
    CHECK(zed_row.user.is_admin);
    CHECK(zed_row.user.last_login_at == T0 + 1);
    CHECK(zed_row.message_count == 1);
    CHECK(zed_row.storage_bytes == 1000);
    CHECK(zed_row.aliases.empty());

    const auto one = repo::get_user_admin(c, amy);
    REQUIRE(one);
    CHECK(one->storage_bytes == amy_row.storage_bytes);
    CHECK(one->aliases.size() == 2);
    CHECK_FALSE(repo::get_user_admin(c, 9999));
  });
}

TEST_CASE("admin webhook events: newest first, cursor paging, type filter", "[accounts][admin]") {
  Fx fx;
  fx.write([&](db::Tx& tx) {
    for (int i = 1; i <= 5; ++i)
      tx.run(
          "INSERT INTO webhook_events(svix_id, type, resend_email_id, payload, received_at, processed_at, "
          "result) VALUES(?,?,?,'{\"secret\":\"body\"}',?,?,?)",
          "msg_" + std::to_string(i), i % 2 ? "email.delivered" : "email.received",
          i == 3 ? std::optional<std::string>() : std::optional<std::string>("re_" + std::to_string(i)),
          T0 + i, i == 5 ? std::optional<int64_t>() : std::optional<int64_t>(T0 + i + 1),
          i == 5 ? std::optional<std::string>() : std::optional<std::string>("applied"));
  });
  fx.read([&](db::Conn& c) {
    auto p1 = repo::list_webhook_events(c, std::nullopt, std::nullopt, 2);
    REQUIRE(p1.items.size() == 2);
    CHECK(p1.items[0].svix_id == "msg_5");
    CHECK_FALSE(p1.items[0].processed_at);
    CHECK_FALSE(p1.items[0].result);
    CHECK(p1.items[1].svix_id == "msg_4");
    REQUIRE(p1.next_cursor);
    auto p2 = repo::list_webhook_events(c, std::nullopt, *p1.next_cursor, 2);
    REQUIRE(p2.items.size() == 2);
    CHECK(p2.items[0].svix_id == "msg_3");
    CHECK_FALSE(p2.items[0].resend_email_id);
    auto p3 = repo::list_webhook_events(c, std::nullopt, *p2.next_cursor, 2);
    REQUIRE(p3.items.size() == 1);
    CHECK(p3.items[0].svix_id == "msg_1");
    CHECK(p3.items[0].resend_email_id == "re_1");
    CHECK(p3.items[0].received_at == T0 + 1);
    CHECK_FALSE(p3.next_cursor);
    // Exactly `limit` rows left → no next cursor.
    CHECK_FALSE(repo::list_webhook_events(c, std::nullopt, std::nullopt, 5).next_cursor);

    auto f = repo::list_webhook_events(c, "email.received", std::nullopt, 50);
    REQUIRE(f.items.size() == 2);
    CHECK(f.items[0].type == "email.received");
    CHECK(repo::list_webhook_events(c, "nope", std::nullopt, 50).items.empty());
  });
  for (const char* bad : {"abc", "-1", "0", "12x", "99999999999999999999"}) {
    INFO(bad);
    CHECK_INVALID_FIELD(fx.read([&](db::Conn& c) { repo::list_webhook_events(c, std::nullopt, bad, 10); }),
                        "cursor");
  }
}

TEST_CASE("admin inbound rows", "[accounts][admin]") {
  Fx fx;
  fx.write([&](db::Tx& tx) {
    insert_inbound(tx, "re_a", "unroutable", std::nullopt, R"(["x@team.example","y@team.example"])");
    insert_inbound(tx, "re_b", "failed", std::nullopt, R"([{"name":"X","email":"z@team.example"}, 5])");
    insert_inbound(tx, "re_c", "delivered", std::nullopt, "not json", T0, T0 - 5);
    insert_inbound(tx, "re_d", "pending", std::nullopt, std::nullopt);
  });
  fx.read([&](db::Conn& c) {
    const auto all = repo::list_inbound(c, std::nullopt, 200);
    REQUIRE(all.size() == 4);
    CHECK(all[0].resend_id == "re_d");
    CHECK(all[0].recipients.empty());
    CHECK(all[1].recipients.empty());  // malformed JSON tolerated
    CHECK(all[1].received_at == T0 - 5);
    CHECK(all[2].recipients == std::vector<std::string>{"z@team.example"});
    CHECK(all[3].recipients == std::vector<std::string>{"x@team.example", "y@team.example"});
    CHECK(all[3].source == "webhook");
    CHECK(all[3].from_email == "x@ext.example");
    CHECK(all[3].subject == "Hi");
    CHECK_FALSE(all[3].message_id_header);

    const auto unr = repo::list_inbound(c, "unroutable", 200);
    REQUIRE(unr.size() == 1);
    CHECK(unr[0].state == "unroutable");
    CHECK(repo::list_inbound(c, std::nullopt, 1).size() == 1);
  });
  CHECK_INVALID_FIELD(fx.read([](db::Conn& c) { repo::list_inbound(c, "bogus", 10); }), "state");
}

TEST_CASE("admin outbox rows", "[accounts][admin]") {
  Fx fx;
  const int64_t u = fx.user("sender@team.example");
  const auto [o1, o2] = fx.write([&](db::Tx& tx) {
    const int64_t alias = test::seed_alias(tx, "help@team.example", {{u, true}});
    const int64_t addr = test::address_id(tx.conn(), "sender@team.example");
    return std::pair{insert_outbound(tx, u, addr, "failed", "u-1"), insert_outbound(tx, u, alias, "queued", "u-2")};
  });
  fx.read([&](db::Conn& c) {
    const auto all = repo::list_outbox(c, std::nullopt, 200);
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == o2);
    CHECK(all[0].from_email == "help@team.example");
    CHECK(all[0].sender_email == "sender@team.example");
    CHECK(all[1].status == "failed");
    CHECK(all[1].total_bytes == 123);
    CHECK_FALSE(all[1].resend_id);
    const auto failed = repo::list_outbox(c, "failed", 200);
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].uuid == "u-1");
    auto row = repo::get_outbox_row(c, o1);
    REQUIRE(row);
    CHECK(row->sender_user_id == u);
    CHECK_FALSE(repo::get_outbox_row(c, 999));
  });
  CHECK_INVALID_FIELD(fx.read([](db::Conn& c) { repo::list_outbox(c, "lost", 10); }), "status");
}

TEST_CASE("admin job rows", "[accounts][admin]") {
  Fx fx;
  const auto [j1, j2] = fx.write([&](db::Tx& tx) {
    const int64_t a = jobs::enqueue(tx, jobs::kinds::kInboundFetch, {{"resend_id", "re_1"}, {"source", "poll"}},
                                    {.dedupe_key = "in:re_1", .now_ms = T0});
    const int64_t b = jobs::enqueue(tx, jobs::kinds::kPollReceiving, {}, {.now_ms = T0});
    tx.run("UPDATE jobs SET state='dead', last_error='boom', payload='not json' WHERE id=?", b);
    return std::pair{a, b};
  });
  fx.read([&](db::Conn& c) {
    const auto all = repo::list_jobs(c, std::nullopt, 200);
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == j2);
    CHECK(all[0].payload.empty());  // unparseable payload → {}
    CHECK(all[0].last_error == "boom");
    CHECK(all[1].kind == "inbound.fetch");
    CHECK(all[1].lane == "inbound");
    CHECK(all[1].dedupe_key == "in:re_1");
    CHECK(json::value(all[1].payload) == json::parse(R"({"resend_id":"re_1","source":"poll"})"));
    CHECK(all[1].run_at == T0);
    CHECK(all[1].attempts == 0);
    CHECK(all[1].max_attempts == 8);
    CHECK_FALSE(all[1].locked_until);
    const auto dead = repo::list_jobs(c, "dead", 200);
    REQUIRE(dead.size() == 1);
    CHECK(dead[0].state == "dead");
    CHECK(repo::get_job(c, j1)->state == "pending");
    CHECK_FALSE(repo::get_job(c, 999));
  });
  CHECK_INVALID_FIELD(fx.read([](db::Conn& c) { repo::list_jobs(c, "zombie", 10); }), "state");
}

TEST_CASE("admin stats", "[accounts][admin]") {
  Fx fx;
  const int64_t now = T0 + 10 * kDay;
  auto st = fx.read([&](db::Conn& c) { return repo::admin_stats(c, now); });
  CHECK(st.users == 0);
  CHECK(st.messages == 0);
  CHECK(st.storage_bytes == 0);
  CHECK_FALSE(st.last_webhook_at);
  CHECK_FALSE(st.last_poll_at);
  CHECK_FALSE(st.quota_blocked);

  const int64_t u = fx.user("u@team.example");
  fx.write([&](db::Tx& tx) {
    const int64_t addr = test::address_id(tx.conn(), "u@team.example");
    insert_message(tx, u, insert_thread(tx, u));
    insert_blob(tx, sha_of('1'), 100);
    insert_blob(tx, sha_of('2'), 23);
    insert_outbound(tx, u, addr, "delivered", "o1", now, now - kHour);       // sent in window
    insert_outbound(tx, u, addr, "delivered", "o2", now, now - 2 * kDay);    // sent long ago
    insert_outbound(tx, u, addr, "failed", "o3", now - kHour);               // failed in window
    insert_outbound(tx, u, addr, "failed", "o4", now - 3 * kDay);            // failed long ago
    insert_inbound(tx, "r1", "delivered", std::nullopt, std::nullopt, now - kHour);
    insert_inbound(tx, "r2", "delivered", std::nullopt, std::nullopt, now - 2 * kDay);
    insert_inbound(tx, "r3", "unroutable", std::nullopt, std::nullopt, now - kHour);
    const int64_t j = jobs::enqueue(tx, jobs::kinds::kPollReceiving, {}, {.now_ms = now});
    jobs::enqueue(tx, jobs::kinds::kGcBlobs, {}, {.now_ms = now});
    tx.run("UPDATE jobs SET state='running' WHERE id=?", j);
    const int64_t d = jobs::enqueue(tx, jobs::kinds::kDbOptimize, {}, {.now_ms = now});
    tx.run("UPDATE jobs SET state='dead' WHERE id=?", d);
    db::kv_set_i64(tx, db::kv_keys::kLastWebhookAt, now - 5, now);
    db::kv_set_i64(tx, db::kv_keys::kLastPollAt, now - 6, now);
    db::kv_set(tx, db::kv_keys::kQuotaBlocked, "daily", now);
  });
  st = fx.read([&](db::Conn& c) { return repo::admin_stats(c, now); });
  CHECK(st.users == 1);
  CHECK(st.messages == 1);
  CHECK(st.storage_bytes == 123);
  CHECK(st.storage.blob_count == 2);
  CHECK(st.storage.blob_bytes == 123);
  CHECK(st.queue_pending == 2);
  CHECK(st.queue_dead == 1);
  CHECK(st.sent_24h == 1);
  CHECK(st.failed_24h == 1);
  CHECK(st.received_24h == 1);
  CHECK(st.last_webhook_at == now - 5);
  CHECK(st.last_poll_at == now - 6);
  CHECK(st.quota_blocked);
}
