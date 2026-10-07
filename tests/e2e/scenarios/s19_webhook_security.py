"""E2E 19 — webhook security: bad signature → 401, timestamp ±6 min → 401, oversized → 413.

A correctly signed event for an unknown email id is accepted (200 {ok:true}) and recorded as
``ignored_unknown`` (DESIGN B9); replaying its svix-id is harmless.
"""

from __future__ import annotations

import json
import time

from lib import flows
from lib.api import http_request, raw_request
from lib.env import Ctx
from lib.wait import eventually
from tools.mock_resend import svix

TITLE = "webhook security: signature, ±6 min timestamps, 1 MiB limit, unknown ids ignored"


def run(ctx: Ctx) -> None:
    env = ctx.env
    url = f"{env.api_base}/api/webhooks/resend"
    tok = ctx.uniq("S19").lower()
    body = json.dumps({"type": "email.delivered", "created_at": "2026-10-07T12:00:00.000Z",
                       "data": {"email_id": f"unknown-{tok}", "created_at": "2026-10-07T12:00:00.000Z",
                                "from": "x@corp.test", "to": ["y@ext.test"], "subject": "unknown",
                                "tags": {}}}, separators=(",", ":"))
    svix_id = svix.new_msg_id()

    def post(payload: str, sid: str | None, ts: int | str | None, sig: str | None):
        headers = {"Content-Type": "application/json"}
        if sid is not None:
            headers["svix-id"] = sid
        if ts is not None:
            headers["svix-timestamp"] = str(ts)
        if sig is not None:
            headers["svix-signature"] = sig
        return http_request("POST", url, data=payload.encode(), headers=headers)

    now = int(time.time())
    good = svix.sign(env.webhook_secret, svix_id, now, body)
    cases = [
        ("wrong secret", body, svix_id, now, svix.sign(svix.new_secret(), svix_id, now, body)),
        ("tampered body", body + " ", svix_id, now, good),
        ("other svix-id", body, svix.new_msg_id(), now, good),
        ("6 min old", body, svix_id, now - 360, svix.sign(env.webhook_secret, svix_id, now - 360, body)),
        ("6 min ahead", body, svix_id, now + 360, svix.sign(env.webhook_secret, svix_id, now + 360, body)),
        ("missing timestamp", body, svix_id, None, good),
        ("timestamp not a number", body, svix_id, "soon", good),
        ("missing signature", body, svix_id, now, None),
        ("missing id", body, None, now, good),
    ]
    for name, payload, sid, ts, sig in cases:
        r = post(payload, sid, ts, sig)
        assert r.status == 401, f"{name}: {r.status} {r.short()}"
        assert r.error_code == "invalid_signature", f"{name}: {r.short()}"

    status, _, resp = raw_request("127.0.0.1", env.api_port, "POST", "/api/webhooks/resend", {
        "Content-Type": "application/json", "Content-Length": str((1 << 20) + 1),
        "svix-id": svix_id, "svix-timestamp": str(now), "svix-signature": good})
    assert status == 413, (status, resp[:200])

    r = post(body, svix_id, now, f"v1,bogus {good}")  # any matching v1 entry passes
    assert r.status == 200 and r.json() == {"ok": True}, r.short()
    r = post(body, svix_id, now, good)  # replay of the same svix-id
    assert r.status == 200, r.short()

    def recorded() -> dict:
        rows = [row for row in ctx.admin.get("/api/admin/events")["items"] if row["svix_id"] == svix_id]
        assert len(rows) == 1, f"{len(rows)} rows for svix-id {svix_id}"
        return rows[0]

    row = eventually(recorded, 10)
    assert row["type"] == "email.delivered" and row["resend_email_id"] == f"unknown-{tok}", row
    assert row["result"] == "ignored_unknown", row
    typed = ctx.admin.get("/api/admin/events", query={"type": "email.delivered"})["items"]
    assert all(r["type"] == "email.delivered" for r in typed), typed
    assert flows.now_ms() - row["received_at"] < 120_000, row
