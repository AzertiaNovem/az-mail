"""E2E 03 — alice→bob send with 5 s undo.

Mock has nothing before 5 s; then exactly 1 POST with User-Agent + Idempotency-Key; bob gets
``mail.new`` over WS; status → delivered.
"""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx
from lib.wait import eventually, never

TITLE = "send with 5 s undo: nothing before the window, 1 POST (UA + key), WS mail.new, delivered"
SMOKE = True
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    flows.set_undo(ctx, alice, 5)
    ws = ctx.ws(bob.api)
    ready = ws.messages[0]
    assert ready["type"] == "ready" and ready["user_id"] == bob.id, ready
    assert isinstance(ready["server_time"], int), ready

    subject = f"你好 Bob {ctx.uniq('S03')}"
    since = ctx.mark()
    draft = flows.create_draft(alice.api, to=[bob], subject=subject, html="<p>第一封测试邮件</p>")
    assert draft["mode"] == "new" and draft["from_address_id"] == alice.address_id, draft
    assert draft["to"] == [bob.address] and draft["subject"] == subject, draft

    t_send = time.time()
    res = flows.send_draft(alice.api, draft)
    assert res["status"] == "queued", res
    assert 3000 < res["undo_ms"] <= 5000, res
    assert res["scheduled_at"] is None, res
    msg = flows.message(alice.api, res["message_id"])
    assert msg["direction"] == "out" and msg["is_draft"] is False, msg
    assert msg["outbound"]["status"] == "queued" and msg["outbound"]["undo_until"] > flows.now_ms(), msg["outbound"]

    never(lambda: ctx.mock.posts(since, subject=subject), duration=3.0,
          desc="POST /emails during the undo window")

    def one_post() -> dict:
        posts = ctx.mock.posts(since, subject=subject)
        assert posts, "no POST /emails yet"
        return posts

    posts = eventually(one_post, 20, desc="POST /emails after the undo window")
    time.sleep(1.0)
    posts = ctx.mock.posts(since, subject=subject)
    assert len(posts) == 1, f"expected exactly 1 POST, got {len(posts)}"
    post = posts[0]
    assert post["status"] == 200, post
    assert post["at"] - t_send >= 4.0, f"POST came {post['at'] - t_send:.1f}s after send (undo is 5 s)"
    assert post["user_agent"] and "python" not in post["user_agent"].lower(), post["user_agent"]
    key = post["idempotency_key"]
    assert key, "missing Idempotency-Key"
    body = post["body"]
    assert alice.email in body["from"], body["from"]
    assert [a for a in body["to"] if bob.email in a], body["to"]
    assert body["subject"] == subject
    assert "第一封测试邮件" in body["html"] and body.get("text"), body
    headers = {k.lower(): v for k, v in (body.get("headers") or {}).items()}
    assert headers.get("x-azmail-ref") == key, f"X-AzMail-Ref must be the outbound uuid: {headers}"
    assert {"name": "azmail_outbound", "value": key} in (body.get("tags") or []), body.get("tags")
    assert len(ctx.mock.emails(since, subject)) == 1

    ev = ws.wait_for(lambda m: isinstance(m, dict) and m.get("type") == "mail.new"
                     and m.get("subject") == subject, timeout=25)
    assert ev["from"]["email"] == alice.email, ev
    assert ev["in_inbox"] is True and ev["is_spam"] is False, ev
    assert isinstance(ev["thread_id"], int) and isinstance(ev["message_id"], int), ev
    assert isinstance(ev["snippet"], str), ev
    assert "data" not in ev, "WS frames are flat (Addendum B)"

    item = flows.wait_thread(bob.api, subject, "inbox")
    assert item["unread"] is True and item["in_inbox"] is True, item
    assert any(p["email"] == alice.email for p in item["participants"]), item["participants"]
    bmsg = flows.message(bob.api, ev["message_id"])
    assert bmsg["direction"] == "in" and bmsg["from"]["email"] == alice.email, bmsg
    assert [a["email"] for a in bmsg["to"]] == [bob.email], bmsg["to"]
    mock_email = ctx.mock.emails(since, subject)[0]
    assert bmsg["message_id_header"].strip("<>") == mock_email["message_id"].strip("<>"), \
        (bmsg["message_id_header"], mock_email["message_id"])

    delivered = flows.wait_status(alice.api, res["message_id"], "delivered", 30)
    assert delivered["outbound"]["sent_at"] is not None, delivered["outbound"]
    events = alice.api.get(f"/api/messages/{res['message_id']}/events")["events"]
    types = [e["type"] for e in events]
    assert "email.sent" in types and "email.delivered" in types, types
    assert all(isinstance(e["occurred_at"], int) and isinstance(e["detail"], dict) for e in events), events
    assert flows.threads_with_subject(alice.api, subject, "sent"), "thread missing from alice's sent folder"
