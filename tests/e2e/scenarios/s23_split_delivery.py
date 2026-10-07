"""E2E 23 — split_delivery: one Resend email per envelope recipient (same Message-ID) → one copy
per owner, also when an owner is reached directly and through an alias (DESIGN C1/C8)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "split delivery: per-recipient Resend emails dedupe to one copy per owner"
TIMEOUT = 90


def _one_copy(user, subject: str) -> None:
    threads = flows.threads_with_subject(user.api, subject, "all")
    assert len(threads) == 1, f"{user.key}: {len(threads)} threads"
    msgs = [m for m in flows.real_messages(flows.thread(user.api, threads[0]["id"])) if m["subject"] == subject]
    assert len(msgs) == 1, f"{user.key}: {len(msgs)} copies"


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob, carol = team.alice, team.bob, team.carol
    ctx.mock.config(split_delivery=True)
    tok = ctx.uniq("S23")
    subject = f"拆分投递 {tok}"
    inj = ctx.mock.inbound(from_=f"split.{tok.lower()}@{EXTERNAL}", to=[alice.email, bob.email],
                           cc=[team.alias_email], subject=subject, text="同一封邮件三次投递")
    assert len(inj["ids"]) == 3, inj
    for user in (alice, bob):
        flows.wait_thread(user.api, subject, "inbox", 30)
    time.sleep(3)  # let all three inbound jobs finish
    for user in (alice, bob):
        eventually(lambda u=user: _one_copy(u, subject), 15)
    via_alias = flows.inbox_copy(bob, subject)
    assert via_alias["delivered_to"] in (bob.email, team.alias_email), via_alias["delivered_to"]

    subject2 = f"拆分回环 {tok}"
    flows.send(carol.api, to=[alice, bob], subject=subject2, html="<p>内部拆分</p>")
    for user in (alice, bob):
        flows.wait_thread(user.api, subject2, "inbox", 30)
    time.sleep(3)
    for user in (alice, bob):
        eventually(lambda u=user: _one_copy(u, subject2), 15)
