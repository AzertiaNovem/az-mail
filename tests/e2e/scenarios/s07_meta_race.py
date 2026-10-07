"""E2E 07 — race: meta_delay=10 and the external reply is injected before message_id capture.

The reply references a Message-ID AZ Mail does not know yet, so it starts a thread of its own;
once the id is captured (webhook without message_id → outbound.fetch_meta / reconcile) the threads
are merged by reference (DESIGN B2).

The partner retitles the reply (no reply prefix). A "Re: <subject>" reply from a participant of
the sent thread is joined to that thread at once by the subject fallback (C7), so the race this
scenario is about would never happen — and which subject the joined thread then showed depended
on whether the send and the reply fell into the same wall-clock second.
"""

from __future__ import annotations

import json
from typing import Any

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
    reply_subject = f"报价确认 {tok}"  # no reply prefix: only the references link it to `subject`
    since = ctx.mark()
    sent = flows.send(alice.api, to=[partner], subject=subject, html="<p>请查收报价</p>")
    email = flows.wait_mock_email(ctx, subject, since)[0]
    msgid = email["message_id"]

    # Resend (the mock) does not reveal the id yet
    resp = http_request("GET", f"{ctx.env.mock_base}/emails/{email['id']}",
                        headers={"Authorization": f"Bearer {ctx.env.resend_key}"})
    assert resp.status == 200 and json.loads(resp.body)["message_id"] is None, resp.short()

    ctx.mock.inbound(from_=f"Partner <{partner}>", to=[alice.email], subject=reply_subject,
                     text="价格可以接受", in_reply_to=msgid, references=msgid)

    def reply_in_inbox() -> dict[str, Any]:
        # By message, not by thread subject: the reply may already sit in the merged thread.
        for item in flows.list_threads(alice.api, folder="inbox"):
            if item["subject"] in (subject, reply_subject):
                for m in flows.real_messages(flows.thread(alice.api, item["id"])):
                    if m["direction"] == "in" and m["subject"] == reply_subject:
                        return m
        raise AssertionError(f"reply {reply_subject!r} not in alice's inbox")

    reply = eventually(reply_in_inbox, 30, 0.4, "external reply in alice's inbox")
    reply = flows.message(alice.api, reply["id"])
    out = flows.message(alice.api, sent["message_id"])
    if not out["message_id_header"]:
        # Read after the reply: the id was not captured when the reply was read either, and
        # nothing else links the two (no reply prefix, so no subject fallback).
        assert reply["thread_id"] != out["thread_id"], \
            "reply joined the sent thread before its Message-ID was captured"

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
    msgs = flows.real_messages(detail)
    assert [m["direction"] for m in msgs] == ["out", "in"], [(m["direction"], m["date"]) for m in msgs]
    assert detail["subject"] == subject, detail["subject"]
    remaining = [t for t in flows.list_threads(alice.api, folder="all")
                 if t["subject"] in (subject, reply_subject)]
    assert len(remaining) == 1, f"expected one merged thread, found {[(t['id'], t['subject']) for t in remaining]}"
