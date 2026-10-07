"""E2E 07 — race: meta_delay=10 and the external reply is injected before message_id capture.

The reply references a Message-ID AZ Mail does not know yet; once the id is captured (webhook
without message_id → outbound.fetch_meta / reconcile) the threads are merged (DESIGN B2).
"""

from __future__ import annotations

import json

from lib import flows
from lib.api import http_request
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "meta_delay race: reply before message_id capture is merged after capture"
TIMEOUT = 150


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice = team.alice
    ctx.mock.config(meta_delay=10)
    tok = ctx.uniq("S07")
    partner = f"partner.{tok.lower()}@{EXTERNAL}"
    subject = f"报价单 {tok}"
    since = ctx.mark()
    sent = flows.send(alice.api, to=[partner], subject=subject, html="<p>请查收报价</p>")
    email = flows.wait_mock_email(ctx, subject, since)[0]
    msgid = email["message_id"]

    # Resend (the mock) does not reveal the id yet
    resp = http_request("GET", f"{ctx.env.mock_base}/emails/{email['id']}",
                        headers={"Authorization": f"Bearer {ctx.env.resend_key}"})
    assert resp.status == 200 and json.loads(resp.body)["message_id"] is None, resp.short()

    ctx.mock.inbound(from_=f"Partner <{partner}>", to=[alice.email], subject="Re: " + subject,
                     text="价格可以接受", in_reply_to=msgid, references=msgid)
    reply = flows.inbox_copy(alice, "Re: " + subject, timeout=30)

    def merged() -> dict:
        out = flows.message(alice.api, sent["message_id"])
        assert out["message_id_header"], "message_id not captured yet"
        rep = flows.message(alice.api, reply["id"])
        assert rep["thread_id"] == out["thread_id"], \
            f"reply thread {rep['thread_id']} != sent thread {out['thread_id']}"
        return out

    out = eventually(merged, 100, 1.0, "threads merged after message_id capture")
    assert out["message_id_header"].strip("<>") == msgid.strip("<>"), (out["message_id_header"], msgid)
    detail = flows.thread(alice.api, out["thread_id"])
    assert sorted(m["direction"] for m in flows.real_messages(detail)) == ["in", "out"], detail
    remaining = [t for t in flows.list_threads(alice.api, folder="all")
                 if t["subject"] in (subject, "Re: " + subject)]
    assert len(remaining) == 1, f"expected one merged thread, found {[(t['id'], t['subject']) for t in remaining]}"
