"""E2E 28 — auth: login throttling 429; logout invalidates; password change revokes the other
sessions and closes their WS; disabled users; WS auth failures close with 4401 (DESIGN D2, A4)."""

from __future__ import annotations

from lib import flows
from lib.env import DOMAIN, Ctx

TITLE = "auth: throttling, logout, password change revokes sessions + WS, disabled user, WS 4401"
TIMEOUT = 90


def _revoked(ws) -> None:
    """The Hub sends session.revoked and closes with 4401 (CONTRACTS §H 16)."""
    code = ws.wait_closed(15)
    assert code == 4401, f"close code {code} (messages: {ws.messages})"


def run(ctx: Ctx) -> None:
    team = ctx.team()
    admin = ctx.admin
    tok = ctx.uniq("s28").lower()

    # throttling: 5 failures per email per window, then 429 with retry_after
    t_email = f"throttle.{tok}@{DOMAIN}"
    t_pw = flows.new_password()
    t_user = admin.post("/api/admin/users", {"email": t_email, "display_name": "限流", "password": t_pw}, expect=201)
    ctx.defer(lambda: admin.request("DELETE", f"/api/admin/users/{t_user['id']}"))
    for i in range(5):
        r = ctx.api.call("POST", "/api/auth/login", {"email": t_email, "password": f"wrong-{i}"})
        assert r.status == 401 and r.error_code == "invalid_credentials", (i, r.short())
    r = ctx.api.call("POST", "/api/auth/login", {"email": t_email, "password": "wrong-again"})
    assert r.status == 429 and r.error_code == "too_many_attempts", r.short()
    assert isinstance(r.error_details.get("retry_after"), int) and r.error_details["retry_after"] > 0, r.short()
    r = ctx.api.call("POST", "/api/auth/login", {"email": t_email, "password": t_pw})
    assert r.status == 429, "the correct password is throttled too while the window lasts"

    # logout invalidates the token
    carol_api, _ = ctx.api.login(team.carol.email, team.carol.password)
    r = carol_api.call("POST", "/api/auth/logout")
    assert r.status == 204 and r.body == b"", r.short()
    r = carol_api.call("GET", "/api/auth/me")
    assert r.status == 401 and r.error_code == "unauthorized", r.short()
    assert team.carol.api.request("GET", "/api/auth/me").status == 200, "other sessions survive a logout"

    # password change revokes the OTHER sessions and closes their WebSockets
    dave = team.dave
    other_api, _ = ctx.api.login(dave.email, dave.password)
    ws_other = ctx.ws(other_api)
    r = dave.api.call("POST", "/api/auth/password", {"current_password": "not-it", "new_password": flows.new_password()})
    assert r.status == 403 and r.error_code == "invalid_credentials", r.short()
    r = dave.api.call("POST", "/api/auth/password", {"current_password": dave.password, "new_password": "1"})
    assert r.status == 422 and r.error_code == "weak_password", r.short()
    new_pw = flows.new_password()
    r = dave.api.call("POST", "/api/auth/password", {"current_password": dave.password, "new_password": new_pw})
    assert r.status == 204, r.short()
    old_pw, dave.password = dave.password, new_pw
    assert other_api.call("GET", "/api/auth/me").status == 401, "other session must be revoked"
    _revoked(ws_other)
    assert dave.api.call("GET", "/api/auth/me").status == 200, "the current session stays valid"
    assert ctx.api.call("POST", "/api/auth/login", {"email": dave.email, "password": old_pw}).status == 401
    ctx.api.login(dave.email, new_pw)

    # disabling a user revokes everything and blocks login
    f_email = f"frank.{tok}@{DOMAIN}"
    f_pw = flows.new_password()
    frank = admin.post("/api/admin/users", {"email": f_email, "display_name": "Frank", "password": f_pw}, expect=201)
    ctx.defer(lambda: admin.request("DELETE", f"/api/admin/users/{frank['id']}"))
    f_api, _ = ctx.api.login(f_email, f_pw)
    ws_frank = ctx.ws(f_api)
    row = admin.patch(f"/api/admin/users/{frank['id']}", {"disabled": True})
    assert row["disabled"] is True, row
    assert f_api.call("GET", "/api/auth/me").status == 401
    _revoked(ws_frank)
    r = ctx.api.call("POST", "/api/auth/login", {"email": f_email, "password": f_pw})
    assert r.status == 403 and r.error_code == "account_disabled", r.short()

    # deleting yourself / the last admin is refused
    me = admin.get("/api/auth/me")
    r = admin.call("DELETE", f"/api/admin/users/{me['id']}")
    assert r.status == 409 and r.error_code in ("cannot_delete_self", "last_admin"), r.short()

    # WebSocket auth failures → close 4401
    ws_bad = ctx.ws(None)
    ws_bad.send_json({"type": "auth", "token": "bogus-token"})
    assert ws_bad.wait_closed(10) == 4401, ws_bad.close_code
    ws_silent = ctx.ws(None)
    assert ws_silent.wait_closed(12) == 4401, f"no auth within 5 s must close 4401 ({ws_silent.close_code})"
