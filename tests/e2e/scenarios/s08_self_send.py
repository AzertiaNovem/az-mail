"""E2E 08 — strip_custom_headers + send to self: still merged by message_id; no duplicate.

The loopback copy lacks X-AzMail-Ref, so AZ Mail must recognise its own mail by Message-ID
(CONTRACTS §H 27): the out copy gets in_inbox=1 and no second 'in' copy remains.
"""

from __future__ import annotations

import time

from lib import flows
from lib.env import Ctx
from lib.wait import eventually

TITLE = "send to self with stripped custom headers: one copy, in inbox"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    ctx.mock.config(strip_custom_headers=True)
    subject = f"备忘 {ctx.uniq('S08')}"
    since = ctx.mark()
    res = flows.send(alice.api, to=[alice], subject=subject, html="<p>给自己的备忘</p>")
    email = flows.wait_mock_email(ctx, subject, since)[0]
    assert {k.lower() for k in email["headers"]} >= {"x-azmail-ref"}, "AZ Mail must still send X-AzMail-Ref"

    def loopback() -> dict:
        recs = [r for r in ctx.mock.received(since) if r["sent_email_id"] == email["id"]]
        assert recs, "no loopback copy at the mock yet"
        return recs[0]

    rec = eventually(loopback, 15)
    assert "x-azmail-ref" not in rec["headers"], "mock did not strip X-AzMail-Ref"

    def single_copy() -> dict:
        threads = flows.threads_with_subject(alice.api, subject, "all")
        assert len(threads) == 1, f"{len(threads)} threads for {subject!r}"
        msgs = flows.real_messages(flows.thread(alice.api, threads[0]["id"]))
        assert len(msgs) == 1, f"{len(msgs)} messages (duplicate loopback copy?)"
        assert msgs[0]["direction"] == "out" and msgs[0]["in_inbox"] is True, msgs[0]
        assert flows.threads_with_subject(alice.api, subject, "inbox"), "not in the inbox"
        return msgs[0]

    msg = eventually(single_copy, 30, desc="self-sent mail merged into one inbox copy")
    assert msg["id"] == res["message_id"], (msg["id"], res["message_id"])
    time.sleep(3.0)  # late inbound processing must not resurrect a duplicate
    single_copy()
    flows.wait_status(alice.api, res["message_id"], "delivered", 30)
