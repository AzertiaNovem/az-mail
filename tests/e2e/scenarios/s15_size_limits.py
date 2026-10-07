"""E2E 15 — upload > 25 MiB → 413 (before the body is read); message > 28 MiB → 413 message_too_large."""

from __future__ import annotations

import os

from lib import flows
from lib.api import raw_request
from lib.env import Ctx
from lib.wait import never

TITLE = "size limits: 25 MiB upload cap (413), 28 MiB per message (413 message_too_large)"
TIMEOUT = 180
MiB = 1 << 20


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    port = ctx.env.api_port

    status, headers, body = raw_request(
        "127.0.0.1", port, "POST", "/api/attachments?filename=big.bin&inline=0",
        {"Authorization": f"Bearer {alice.api.token}", "Content-Type": "application/octet-stream",
         "Content-Length": str(25 * MiB + 1)})
    assert status == 413, (status, body[:200])
    assert b"payload_too_large" in body, body[:200]

    exactly = flows.upload(alice.api, "exactly-25MiB.bin", "application/octet-stream",
                           os.urandom(1024) * (25 * 1024))
    assert exactly["size"] == 25 * MiB, exactly
    extra = flows.upload(alice.api, "extra-4MiB.bin", "application/octet-stream", os.urandom(4 * MiB))

    subject = f"超大邮件 {ctx.uniq('S15')}"
    since = ctx.mark()
    draft = flows.create_draft(alice.api, to=[bob], subject=subject,
                               attachment_ids=[exactly["id"], extra["id"]])
    r = flows.send_draft(alice.api, draft, expect=None)
    assert r.status == 413 and r.error_code == "message_too_large", r.short()
    assert alice.api.get(f"/api/drafts/{draft['id']}")["id"] == draft["id"]
    never(lambda: ctx.mock.requests(since, method="POST", path="/emails"), 2.0,
          desc="an oversized message reaching the mock")
