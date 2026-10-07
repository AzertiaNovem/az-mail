"""E2E 16 — scheduled send without attachments: scheduled_via=resend; reschedule PATCH; cancel →
draft; advance → sent (DESIGN B5)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx
from lib.wait import eventually

TITLE = "schedule via Resend: ISO scheduled_at, reschedule PATCH, cancel → draft, advance → sent"
TIMEOUT = 150

HOUR = 3600 * 1000


def _iso_ms(value: str) -> int:
    from tools.mock_resend.server import parse_iso

    return int(round(parse_iso(value) * 1000))


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    subject = f"定时发送 {ctx.uniq('S16')}"
    at = (flows.now_ms() // 1000) * 1000 + HOUR
    since = ctx.mark()
    draft = flows.create_draft(alice.api, to=[bob], subject=subject, html="<p>一小时后发送</p>")
    res = flows.send_draft(alice.api, draft, scheduled_at=at)
    assert res["scheduled_at"] == at, res
    mid = res["message_id"]

    msg = flows.wait_status(alice.api, mid, "scheduled", 20)
    assert msg["outbound"]["scheduled_via"] == "resend", msg["outbound"]
    assert msg["outbound"]["scheduled_at"] == at, msg["outbound"]
    post = [p for p in ctx.mock.posts(since, subject=subject) if p["status"] == 200]
    assert len(post) == 1, post
    sched = post[0]["body"]["scheduled_at"]
    assert isinstance(sched, str) and sched.endswith("Z"), f"scheduled_at must be ISO UTC …Z: {sched!r}"
    assert abs(_iso_ms(sched) - at) < 1000, (sched, at)
    email = ctx.mock.emails(since, subject)[0]
    assert email["last_event"] == "scheduled" and email["attachments"] == [], email
    assert flows.threads_with_subject(alice.api, subject, "scheduled"), "not in the scheduled folder"
    assert not flows.threads_with_subject(alice.api, subject, "sent"), "scheduled mail is not 'sent' yet"

    # reschedule → PATCH /emails/{id}
    at2 = at + HOUR
    mark = ctx.mark()
    resched = alice.api.post(f"/api/messages/{mid}/reschedule", {"scheduled_at": at2})
    assert resched["outbound"]["scheduled_at"] == at2, resched["outbound"]
    patches = ctx.mock.requests(mark, method="PATCH")
    assert len(patches) == 1 and patches[0]["path"] == f"/emails/{email['id']}", patches
    assert abs(_iso_ms(patches[0]["body"]["scheduled_at"]) - at2) < 1000, patches[0]["body"]
    assert abs(ctx.mock.email(email["id"])["scheduled_at"] * 1000 - at2) < 1000
    for bad in (flows.now_ms() - 1000, flows.now_ms() + 40 * 24 * HOUR):
        r = alice.api.call("POST", f"/api/messages/{mid}/reschedule", {"scheduled_at": bad})
        assert r.status == 422 and r.error_code == "invalid_schedule", r.short()

    # cancel → POST /emails/{id}/cancel → back to a draft
    mark = ctx.mark()
    canceled = alice.api.post(f"/api/messages/{mid}/cancel-schedule")["draft"]
    assert canceled["subject"] == subject, canceled
    cancels = ctx.mock.requests(mark, method="POST", path=f"/emails/{email['id']}/cancel")
    assert len(cancels) == 1 and cancels[0]["status"] == 200, cancels
    assert ctx.mock.email(email["id"])["last_event"] == "canceled"
    flows.wait_thread(alice.api, subject, "drafts", 10)
    assert not flows.threads_with_subject(alice.api, subject, "scheduled")

    # schedule again and let the mock clock pass the time
    mark = ctx.mark()
    res2 = flows.send_draft(alice.api, canceled, scheduled_at=flows.now_ms() + HOUR)
    flows.wait_status(alice.api, res2["message_id"], "scheduled", 20)

    def rescheduled_email() -> dict:
        found = ctx.mock.emails(mark, subject)
        assert found, "re-scheduled mail not at the mock"
        return found[0]

    email2 = eventually(rescheduled_email, 10)
    fired = ctx.mock.advance(HOUR // 1000 + 120)["fired"]
    assert email2["id"] in fired, (email2["id"], fired)
    final = flows.wait_status(alice.api, res2["message_id"], {"sent", "delivered"}, 40)
    assert final["outbound"]["sent_at"] is not None or final["outbound"]["status"] == "sent", final["outbound"]
    copy = flows.inbox_copy(bob, subject, timeout=30)
    assert copy["direction"] == "in", copy
    assert not flows.threads_with_subject(alice.api, subject, "scheduled")
    time.sleep(0.5)
