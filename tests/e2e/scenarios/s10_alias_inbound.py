"""E2E 10 — external mail → support@: alice and bob each get a copy (independent read state),
``delivered_to = support@``; non-members get nothing."""

from __future__ import annotations

import time

from lib import flows
from lib.env import EXTERNAL, Ctx

TITLE = "external → alias: per-member copies, independent read state, delivered_to"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    tok = ctx.uniq("S10")
    customer = f"customer.{tok.lower()}@{EXTERNAL}"
    subject = f"订单问题 {tok}"
    ctx.mock.inbound(from_=f"王客户 <{customer}>", to=[team.alias_email], subject=subject,
                     html="<p>我的订单有问题</p>", text="我的订单有问题")

    a = flows.inbox_copy(alice, subject)
    b = flows.inbox_copy(bob, subject)
    assert a["id"] != b["id"], "each member needs an own copy"
    for copy in (a, b):
        assert copy["delivered_to"] == team.alias_email, copy["delivered_to"]
        assert copy["from"]["email"] == customer and copy["from"]["name"] == "王客户", copy["from"]
        assert copy["is_read"] is False and copy["in_inbox"] is True, copy
        assert copy["auth"] and copy["auth"]["dmarc"] == "pass", copy["auth"]

    patched = alice.api.patch(f"/api/messages/{a['id']}", {"is_read": True})
    assert patched["is_read"] is True, patched
    assert flows.message(bob.api, b["id"])["is_read"] is False, "read state must be per copy"
    bob_item = flows.wait_thread(bob.api, subject, "inbox")
    assert bob_item["unread"] is True, bob_item
    alice_item = flows.wait_thread(alice.api, subject, "inbox")
    assert alice_item["unread"] is False, alice_item

    time.sleep(1.5)
    for user in (team.carol, team.dave):
        assert not flows.threads_with_subject(user.api, subject, "all"), f"{user.key} got alias mail"
