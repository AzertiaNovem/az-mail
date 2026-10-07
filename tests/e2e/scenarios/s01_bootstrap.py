"""E2E 01 — bootstrap: migrate, create-user admin (stdin), login, me, health; token works; doctor OK."""

from __future__ import annotations

from lib import flows
from lib.env import ADMIN_EMAIL, Ctx

TITLE = "bootstrap: migrate, create-user (stdin), login, me, health, doctor"
SMOKE = True
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    env = ctx.env
    # migrate is idempotent (the harness already ran it once)
    res = env.cli("migrate")
    assert res.returncode == 0, f"azmail migrate (2nd run) exit {res.returncode}: {res.stderr[-500:]}"

    health = ctx.api.get("/api/health")
    assert health["status"] == "ok", health
    assert health["db"] == "ok", health
    assert isinstance(health["version"], str) and health["version"], health
    assert abs(health["time"] - flows.now_ms()) < 120_000, f"health.time not ms epoch: {health}"

    api, login = ctx.api.login(ADMIN_EMAIL, env.admin_password)
    assert isinstance(login["token"], str) and len(login["token"]) >= 20, login
    assert login["expires_at"] > flows.now_ms(), login
    me = login["user"]
    assert me["email"] == ADMIN_EMAIL, me
    assert me["is_admin"] is True, me
    assert me["display_name"] == "管理员", me
    settings = me["settings"]
    for key in ("undo_send_seconds", "signature_html", "signature_enabled", "timezone", "page_size",
                "remote_images", "trusted_image_senders", "display_name"):
        assert key in settings, f"Settings lacks {key}: {settings}"
    own = [i for i in me["identities"] if i["email"] == ADMIN_EMAIL]
    assert len(own) == 1 and own[0]["kind"] == "user" and own[0]["is_default"] is True, me["identities"]
    server = me["server"]
    assert server["blob_backend"] == env.blob_backend, server
    assert env.api_base in server["files_origins"], server
    assert server["version"] == health["version"], (server, health)

    # the token works; missing / bogus tokens are 401 with the error envelope
    again = api.get("/api/auth/me")
    assert again["id"] == me["id"] and again["email"] == ADMIN_EMAIL, again
    for anon in (ctx.api, ctx.api.with_token("not-a-real-token")):
        r = anon.call("GET", "/api/auth/me")
        assert r.status == 401 and r.error_code == "unauthorized", r.short()
        err = r.json()["error"]
        assert isinstance(err["message"], str) and err["message"], err
    r = ctx.api.call("POST", "/api/auth/login", {"email": ADMIN_EMAIL, "password": "wrong-password"})
    assert r.status == 401 and r.error_code == "invalid_credentials", r.short()

    # standard response headers
    r = api.call("GET", "/api/auth/me")
    assert r.header("x-request-id"), r.headers
    assert "no-store" in (r.header("cache-control") or ""), r.headers
    assert r.header("x-content-type-options") == "nosniff", r.headers
    assert (r.header("content-type") or "").startswith("application/json"), r.headers

    # unknown route → 404, wrong method → 405 with Allow
    r = api.call("GET", "/api/definitely-not-a-route")
    assert r.status == 404 and r.error_code == "not_found", r.short()
    r = api.call("DELETE", "/api/health")
    assert r.status == 405 and r.error_code == "method_not_allowed", r.short()
    assert "GET" in (r.header("allow") or ""), r.headers

    # create-user for an existing address fails and leaves the password untouched
    res = env.cli("create-user", "--email", ADMIN_EMAIL, "--admin", stdin="Other-Passw0rd-123\n")
    assert res.returncode != 0, "create-user of an existing address must fail"
    ctx.api.login(ADMIN_EMAIL, env.admin_password)

    res = env.cli("doctor")
    assert res.returncode == 0, f"azmail doctor exit {res.returncode}:\n{(res.stdout + res.stderr)[-1500:]}"
    if env.blob_backend == "r2":
        res = env.cli("doctor", "--r2")
        assert res.returncode == 0, f"azmail doctor --r2 exit {res.returncode}:\n{(res.stdout + res.stderr)[-1500:]}"
    for secret in (env.server_secret, env.resend_key, env.s3_secret_key, env.webhook_secret):
        assert secret not in res.stdout + res.stderr, "doctor output leaks a secret"
