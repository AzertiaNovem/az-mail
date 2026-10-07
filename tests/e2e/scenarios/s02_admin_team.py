"""E2E 02 — admin creates domain, alice, bob, carol, dave, alias support@ (alice send-as, bob member).

409 on duplicate addresses. The CLI bootstrap already created corp.test (CONTRACTS §H 37), so
re-creating it is a 409 and an additional domain is created/deleted through the admin API.
"""

from __future__ import annotations

from lib import flows
from lib.env import DOMAIN, EXTRA_DOMAIN, Ctx

TITLE = "admin: domains, users, alias support@ (send-as), duplicate addresses → 409"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    admin = ctx.admin

    r = admin.call("POST", "/api/admin/domains", {"name": DOMAIN})
    assert r.status == 409 and r.error_code == "domain_exists", r.short()

    existing = {d["name"]: d for d in admin.get("/api/admin/domains")}
    if EXTRA_DOMAIN in existing:  # left over from an earlier partial run of this scenario
        admin.delete(f"/api/admin/domains/{existing[EXTRA_DOMAIN]['id']}")
    row = admin.post("/api/admin/domains", {"name": EXTRA_DOMAIN}, expect=201)
    assert row["name"] == EXTRA_DOMAIN and isinstance(row["id"], int), row
    assert isinstance(row["receiving_enabled"], bool) and isinstance(row["created_at"], int), row
    domains = admin.get("/api/admin/domains")
    assert isinstance(domains, list), "GET /api/admin/domains must be a bare array (Addendum B)"
    assert {DOMAIN, EXTRA_DOMAIN} <= {d["name"] for d in domains}, domains
    assert admin.get(f"/api/admin/domains/{row['id']}") == row
    r = admin.call("POST", "/api/admin/domains", {"name": EXTRA_DOMAIN.upper()})
    assert r.status == 409 and r.error_code == "domain_exists", r.short()

    # a user on the extra domain, then the domain is in use
    erin = admin.post("/api/admin/users", {"email": f"erin@{EXTRA_DOMAIN}", "display_name": "Erin",
                                           "password": flows.new_password()}, expect=201)
    r = admin.call("DELETE", f"/api/admin/domains/{row['id']}")
    assert r.status == 409 and r.error_code == "domain_in_use", r.short()
    r = admin.delete(f"/api/admin/users/{erin['id']}")
    assert r.body == b"", "DELETE must return 204 with an empty body"
    admin.delete(f"/api/admin/domains/{row['id']}")
    r = admin.call("GET", f"/api/admin/domains/{row['id']}")
    assert r.status == 404 and r.error_code == "not_found", r.short()

    team = flows.ensure_team(ctx, check=True)
    alice, bob = team.alice, team.bob

    password = flows.new_password()
    for email in (alice.email, alice.email.upper()):
        r = admin.call("POST", "/api/admin/users", {"email": email, "display_name": "dup", "password": password})
        assert r.status == 409 and r.error_code == "address_exists", (email, r.short())
    r = admin.call("POST", "/api/admin/aliases", {"email": alice.email, "display_name": "x",
                                                  "share_sent": True, "members": []})
    assert r.status == 409 and r.error_code == "address_exists", r.short()
    r = admin.call("POST", "/api/admin/users", {"email": team.alias_email, "display_name": "x", "password": password})
    assert r.status == 409 and r.error_code == "address_exists", r.short()
    r = admin.call("POST", "/api/admin/aliases", {"email": team.alias_email, "display_name": "x",
                                                  "share_sent": True, "members": []})
    assert r.status == 409 and r.error_code == "address_exists", r.short()
    r = admin.call("POST", "/api/admin/users", {"email": "x@nowhere.test", "display_name": "x", "password": password})
    assert r.status == 422 and r.error_code == "unknown_domain", r.short()
    r = admin.call("POST", "/api/admin/users", {"email": f"weak@{DOMAIN}", "display_name": "w", "password": "1"})
    assert r.status == 422 and r.error_code == "weak_password", r.short()

    aliases = admin.get("/api/admin/aliases")
    assert isinstance(aliases, list), aliases
    support = next(a for a in aliases if a["email"] == team.alias_email)
    assert support["display_name"] == flows.ALIAS_NAME and support["share_sent"] is True, support
    assert isinstance(support["created_at"], int), support
    members = {(m["user_id"], m["can_send_as"]) for m in support["members"]}
    assert members == {(alice.id, True), (bob.id, False)}, support["members"]
    assert all(m["email"] and "display_name" in m for m in support["members"]), support["members"]

    users = {u["email"]: u for u in admin.get("/api/admin/users")}
    assert users[alice.email]["aliases"] == [{"id": team.alias_id, "email": team.alias_email,
                                              "can_send_as": True}], users[alice.email]
    assert users[bob.email]["aliases"] == [{"id": team.alias_id, "email": team.alias_email,
                                            "can_send_as": False}], users[bob.email]
    assert users[alice.email]["last_login_at"] is not None, "login must record last_login_at"

    ids = alice.api.get("/api/identities")
    assert isinstance(ids, list), ids
    by_email = {i["email"]: i for i in ids}
    assert set(by_email) == {alice.email, team.alias_email}, ids
    assert by_email[alice.email]["kind"] == "user" and by_email[alice.email]["is_default"] is True, ids
    assert by_email[team.alias_email]["kind"] == "alias" and by_email[team.alias_email]["is_default"] is False, ids
    assert by_email[team.alias_email]["display_name"] == flows.ALIAS_NAME, ids
    bob_ids = bob.api.get("/api/identities")
    assert [i["email"] for i in bob_ids] == [bob.email], f"bob may not send as support@: {bob_ids}"
    assert [i["email"] for i in bob.api.get("/api/auth/me")["identities"]] == [bob.email]

    for method, path, body in (("GET", "/api/admin/users", None), ("GET", "/api/admin/stats", None),
                               ("POST", "/api/admin/domains", {"name": "x.test"})):
        r = alice.api.call(method, path, body)
        assert r.status == 403 and r.error_code == "forbidden", (path, r.short())
