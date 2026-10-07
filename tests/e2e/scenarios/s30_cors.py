"""E2E 30 — CORS: allowed-origin preflight; disallowed origin; WebSocket with a bad Origin
(DESIGN A4, http/cors.hpp)."""

from __future__ import annotations

from lib.env import ALLOWED_ORIGIN, SECOND_ORIGIN, Ctx
from lib.ws import WsClient, WsHandshakeError

TITLE = "CORS allowlist for REST and WebSocket upgrades"


def _ws_rejected(ctx: Ctx, origin: str | None, token: str) -> None:
    client = WsClient(ctx.env.ws_url, origin=origin)
    try:
        client.connect()
    except WsHandshakeError as exc:
        assert exc.status in (400, 403), f"unexpected handshake status {exc.status}"
        return
    # accepted at the HTTP level: it must never become usable
    ctx.defer(client.close)
    client.send_json({"type": "auth", "token": token})
    code = client.wait_closed(10)
    assert not any(isinstance(m, dict) and m.get("type") == "ready" for m in client.messages), \
        f"WS with Origin {origin!r} was authenticated"
    assert code is not None


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    api = ctx.api

    for origin in (ALLOWED_ORIGIN, SECOND_ORIGIN):
        r = api.call("OPTIONS", "/api/threads", headers={
            "Origin": origin, "Access-Control-Request-Method": "GET",
            "Access-Control-Request-Headers": "authorization, content-type"})
        assert r.status in (200, 204), (origin, r.status, r.short())
        assert r.header("access-control-allow-origin") == origin, r.headers
        assert "GET" in (r.header("access-control-allow-methods") or ""), r.headers
        assert "authorization" in (r.header("access-control-allow-headers") or "").lower(), r.headers
        assert "origin" in (r.header("vary") or "").lower(), r.headers
        assert r.header("access-control-allow-credentials") in (None, "false"), "no credentials mode"

    r = api.call("OPTIONS", "/api/threads", headers={"Origin": "https://evil.test",
                                                    "Access-Control-Request-Method": "GET"})
    assert r.status == 403, r.short()
    assert r.header("access-control-allow-origin") is None, r.headers
    assert "origin" in (r.header("vary") or "").lower(), r.headers

    r = alice.api.call("GET", "/api/counts", headers={"Origin": ALLOWED_ORIGIN})
    assert r.status == 200 and r.header("access-control-allow-origin") == ALLOWED_ORIGIN, r.headers
    assert "x-request-id" in (r.header("access-control-expose-headers") or "").lower(), r.headers
    r = alice.api.call("GET", "/api/counts", headers={"Origin": "https://evil.test"})
    assert r.header("access-control-allow-origin") is None, r.headers
    assert "origin" in (r.header("vary") or "").lower(), r.headers
    r = api.call("GET", "/api/health", headers={"Origin": "null"})
    assert r.header("access-control-allow-origin") is None, "Origin: null is never allowed"

    _ws_rejected(ctx, "https://evil.test", alice.api.token or "")
    _ws_rejected(ctx, None, alice.api.token or "")
    ok = ctx.ws(alice.api, origin=ALLOWED_ORIGIN)
    ok.send_json({"type": "ping"})
    ok.wait_for("pong", 10)
