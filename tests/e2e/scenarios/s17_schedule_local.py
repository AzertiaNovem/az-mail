"""E2E 17 — scheduled with attachments + reject-scheduled-attachments → local scheduling.

The outbox job's run_at is the scheduled time; Resend receives a plain (unscheduled) POST at
that time. (AZMAIL_SCHEDULE_MIN_LEAD_SEC=5 in the E2E env keeps this short; local scheduling
follows the backend clock, so the mock clock is not involved.)
"""

from __future__ import annotations

import os

from lib import flows
from lib.env import Ctx
from lib.wait import eventually, never

TITLE = "schedule with attachments → scheduled_via=local, plain POST at the scheduled time"
TIMEOUT = 120


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    ctx.mock.config(reject_scheduled_attachments=True)
    data = b"%PDF-1.4 contract " + os.urandom(512)
    att = flows.upload(alice.api, "合同.pdf", "application/pdf", data)
    subject = f"定时带附件 {ctx.uniq('S17')}"
    at = flows.now_ms() + 15_000
    since = ctx.mark()
    res = flows.send(alice.api, to=[bob], subject=subject, html="<p>合同见附件</p>",
                     attachment_ids=[att["id"]], scheduled_at=at)
    assert res["scheduled_at"] == at, res

    def local() -> dict:
        msg = flows.message(alice.api, res["message_id"])
        assert msg["outbound"]["scheduled_via"] == "local", msg["outbound"]
        return msg

    msg = eventually(local, 15, desc="scheduled_via=local")
    assert msg["outbound"]["scheduled_at"] == at, msg["outbound"]
    assert flows.threads_with_subject(alice.api, subject, "scheduled"), "not in the scheduled folder"

    def accepted() -> list:
        return [p for p in ctx.mock.posts(since, subject=subject) if p["status"] == 200]

    def sent_once() -> list:
        found = accepted()
        assert found, "not sent yet"
        return found

    remaining = (at - flows.now_ms()) / 1000.0 - 1.5
    if remaining > 0:
        never(accepted, remaining, desc="an accepted send before the scheduled time")
    posts = eventually(sent_once, 30, desc="the locally scheduled send")
    assert len(posts) == 1, posts
    assert posts[0]["at"] * 1000 >= at - 1500, f"sent {at - posts[0]['at'] * 1000:.0f} ms early"
    assert posts[0]["body"].get("scheduled_at") in (None, ""), "local scheduling must not pass scheduled_at"
    for p in ctx.mock.posts(since, subject=subject):
        assert p["status"] in (200, 422), p  # 422 = the optional Resend scheduling attempt
    email = ctx.mock.emails(since, subject)
    assert len(email) == 1 and email[0]["attachments"][0]["sha256"], email

    flows.wait_status(alice.api, res["message_id"], "delivered", 30)
    copy = flows.inbox_copy(bob, subject, timeout=30)
    assert [a["filename"] for a in copy["attachments"]] == ["合同.pdf"], copy["attachments"]
