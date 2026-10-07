"""E2E 06 — bob replies; alice's thread has 2 messages via In-Reply-To / References."""

from __future__ import annotations

from lib import flows
from lib.env import Ctx
from lib.wait import eventually

TITLE = "reply threads via In-Reply-To/References (2 messages on both sides)"
TIMEOUT = 90


def _norm(msgid: str) -> str:
    return msgid.strip().strip("<>")


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    subject = f"项目讨论 {ctx.uniq('S06')}"
    sent = flows.send(alice.api, to=[bob], subject=subject, html="<p>请看一下方案</p>")
    incoming = flows.inbox_copy(bob, subject)
    assert incoming["message_id_header"], incoming

    since = ctx.mark()
    reply = flows.create_draft(bob.api, mode="reply", parent_message_id=incoming["id"], to=[alice],
                               subject="Re: " + subject, html="<p>收到，没问题</p>")
    assert reply["mode"] == "reply" and reply["parent_message_id"] == incoming["id"], reply
    assert reply["thread_id"] == incoming["thread_id"], "reply draft must live in the parent's thread"
    res = flows.send_draft(bob.api, reply)

    post = flows.wait_mock_email(ctx, "Re: " + subject, since)[0]
    headers = {k.lower(): v for k, v in post["headers"].items()}
    parent_id = _norm(incoming["message_id_header"])
    assert _norm(headers.get("in-reply-to", "")) == parent_id, headers
    assert parent_id in headers.get("references", ""), headers

    def alice_thread() -> dict:
        original = flows.message(alice.api, sent["message_id"])
        detail = flows.thread(alice.api, original["thread_id"])
        msgs = flows.real_messages(detail)
        assert len(msgs) == 2, f"alice's thread has {len(msgs)} message(s)"
        assert sorted(m["direction"] for m in msgs) == ["in", "out"], msgs
        return detail

    detail = eventually(alice_thread, 30, desc="reply joins alice's thread")
    reply_in = next(m for m in detail["messages"] if m["direction"] == "in")
    assert _norm(reply_in["message_id_header"] or "") == _norm(post["message_id"]), reply_in
    items = [t for t in flows.list_threads(alice.api, folder="inbox") if t["id"] == detail["id"]]
    assert items and items[0]["message_count"] == 2 and items[0]["unread"] is True, items

    bob_detail = flows.thread(bob.api, flows.message(bob.api, res["message_id"])["thread_id"])
    assert len(flows.real_messages(bob_detail)) == 2, bob_detail
