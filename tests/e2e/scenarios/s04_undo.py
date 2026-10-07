"""E2E 04 — undo inside the window (draft restored, 0 POSTs) / after it (409 too_late)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx
from lib.wait import never, wait_for

TITLE = "undo inside the window restores the draft (0 POSTs); after it → 409 too_late"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    flows.set_undo(ctx, alice, 5)

    subject = f"撤销测试 {ctx.uniq('S04')}"
    since = ctx.mark()
    draft = flows.create_draft(alice.api, to=[bob], subject=subject, html="<p>撤销我</p>")
    res = flows.send_draft(alice.api, draft)
    time.sleep(0.5)
    undo = alice.api.post(f"/api/messages/{res['message_id']}/undo-send", expect=200)
    restored = undo["draft"]
    assert restored["subject"] == subject and restored["to"] == [bob.address], restored
    assert "撤销我" in restored["html"], restored
    assert restored["mode"] == "new", restored
    got = alice.api.get(f"/api/drafts/{restored['id']}")
    assert got["subject"] == subject, got
    flows.wait_thread(alice.api, subject, "drafts", 10)
    assert not flows.threads_with_subject(alice.api, subject, "sent"), "undone mail shows in sent"
    never(lambda: ctx.mock.posts(since, subject=subject), duration=7.0, desc="POST after undo")
    assert not flows.threads_with_subject(bob.api, subject, "all"), "bob received an undone mail"

    # a second undo of the same message is too late (it is a draft again)
    r = alice.api.call("POST", f"/api/messages/{res['message_id']}/undo-send")
    assert r.status in (404, 409), r.short()

    subject2 = f"撤销太晚 {ctx.uniq('S04')}"
    res2 = flows.send(alice.api, to=[bob], subject=subject2, html="<p>来不及了</p>")
    wait_for(lambda: ctx.mock.posts(since, subject=subject2), 20, desc="POST after the window")
    r = alice.api.call("POST", f"/api/messages/{res2['message_id']}/undo-send")
    assert r.status == 409 and r.error_code == "too_late", r.short()
    flows.wait_status(alice.api, res2["message_id"], "delivered", 30)
    flows.wait_thread(bob.api, subject2, "inbox")
