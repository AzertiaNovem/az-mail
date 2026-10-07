"""E2E 05 — undo → edit → resend: a new outbound uuid (Idempotency-Key), no 409 (DESIGN B1)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx
from lib.wait import eventually

TITLE = "undo → edit → resend uses a new idempotency key (no 409)"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    flows.set_undo(ctx, alice, 5)

    subject = f"改稿 {ctx.uniq('S05')}"
    edited_subject = subject + "（修改）"
    since = ctx.mark()
    draft = flows.create_draft(alice.api, to=[bob], subject=subject, html="<p>初稿</p>")
    first = flows.send_draft(alice.api, draft)
    time.sleep(0.3)
    restored = alice.api.post(f"/api/messages/{first['message_id']}/undo-send", expect=200)["draft"]

    body = flows.draft_body(to=[bob], subject=edited_subject, html="<p>修改后的内容</p>")
    body["version"] = restored["version"]
    edited = alice.api.put(f"/api/drafts/{restored['id']}", body)
    assert edited["subject"] == edited_subject and edited["version"] > restored["version"], edited
    second = flows.send_draft(alice.api, edited)
    assert second["outbound_id"] != first["outbound_id"], "resend must create a new outbound row"

    def posted() -> list:
        posts = ctx.mock.posts(since, subject=edited_subject)
        assert posts, "edited mail not posted yet"
        return posts

    eventually(posted, 20)
    time.sleep(1.0)
    all_posts = ctx.mock.requests(since, method="POST", path="/emails")
    assert len(all_posts) == 1, [(p["status"], (p.get("body") or {}).get("subject")) for p in all_posts]
    post = all_posts[0]
    assert post["status"] == 200, post
    assert (post.get("body") or {}).get("subject") == edited_subject
    assert post["idempotency_key"], post
    assert not [r for r in ctx.mock.requests(since) if r["status"] == 409], "a 409 reached the mock"
    assert ctx.mock.emails(since, subject) == [], "the undone original reached Resend"

    flows.wait_status(alice.api, second["message_id"], "delivered", 30)
    copy = flows.inbox_copy(bob, edited_subject)
    assert "修改后的内容" in (copy["html"] or ""), copy["html"]
    assert not flows.threads_with_subject(bob.api, subject, "all"), "bob got the undone original"
