"""E2E 32 — crash: SIGKILL during the undo window, restart → sent exactly once (DESIGN A6)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx

TITLE = "SIGKILL during the undo window, restart: exactly one send"
TIMEOUT = 120


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    flows.set_undo(ctx, alice, 5)
    subject = f"崩溃恢复 {ctx.uniq('S32')}"
    since = ctx.mark()
    res = flows.send(alice.api, to=[bob], subject=subject, html="<p>重启后再发</p>")
    time.sleep(0.5)
    ctx.env.kill_backend()
    assert ctx.mock.posts(since, subject=subject) == [], "sent before the crash (inside the undo window)"
    ctx.env.start_backend()

    flows.wait_mock_email(ctx, subject, since, timeout=40)
    time.sleep(6)
    emails = ctx.mock.emails(since, subject)
    assert len(emails) == 1, f"{len(emails)} emails after the restart"
    fresh = [p for p in ctx.mock.posts(since, subject=subject) if p["status"] == 200 and not p["replayed"]]
    assert len(fresh) == 1, fresh
    flows.wait_status(alice.api, res["message_id"], "delivered", 30)
    copy = flows.inbox_copy(bob, subject)
    threads = flows.threads_with_subject(bob.api, subject, "all")
    assert len(threads) == 1 and len(flows.real_messages(flows.thread(bob.api, threads[0]["id"]))) == 1, copy
