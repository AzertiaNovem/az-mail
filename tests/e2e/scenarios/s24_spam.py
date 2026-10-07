"""E2E 24 — DMARC fail and spoofed internal From → spam + warnings; not_spam moves to inbox (C11)."""

from __future__ import annotations

from lib import flows
from lib.env import Ctx

TITLE = "spam: dmarc_fail and spoofed_internal warnings; not_spam → inbox"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    tok = ctx.uniq("S24")
    before = flows.counts(alice.api)

    s1 = f"紧急转账 {tok}"
    ctx.mock.inbound(from_="CEO <ceo@evil.test>", to=[alice.email], subject=s1, text="请立即转账",
                     spf="fail", dkim="fail", dmarc="fail")
    m1 = flows.inbox_copy(alice, s1, folder="spam", timeout=30)
    assert m1["is_spam"] is True and m1["in_inbox"] is False, m1
    assert "dmarc_fail" in m1["warnings"], m1["warnings"]
    assert m1["auth"] and m1["auth"]["dmarc"] == "fail", m1["auth"]
    assert not flows.threads_with_subject(alice.api, s1, "inbox"), "DMARC-failing mail in the inbox"

    s2 = f"冒充同事 {tok}"
    ctx.mock.inbound(from_=f"{bob.name} <{bob.email}>", to=[alice.email], subject=s2, text="我是 Bob",
                     spf="pass", dkim="fail", dmarc="unknown")
    m2 = flows.inbox_copy(alice, s2, folder="spam", timeout=30)
    assert m2["is_spam"] is True, m2
    assert "spoofed_internal" in m2["warnings"], m2["warnings"]

    after = flows.counts(alice.api)
    assert after["spam_unread"] >= before["spam_unread"] + 2, (before, after)

    item = flows.wait_thread(alice.api, s1, "spam")
    out = alice.api.post("/api/threads/actions", {"thread_ids": [item["id"]], "action": "not_spam"})
    assert out == {"thread_ids": [item["id"]]}, out
    flows.wait_thread(alice.api, s1, "inbox", 10)
    assert not flows.threads_with_subject(alice.api, s1, "spam")
    assert flows.message(alice.api, m1["id"])["is_spam"] is False
    assert flows.counts(alice.api)["spam_unread"] == after["spam_unread"] - 1
