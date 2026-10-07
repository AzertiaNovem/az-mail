"""E2E 11 — alice replies as support@: bob gets a shared sent copy in the same thread; status
updates reach both copies (DESIGN C3)."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "reply as alias: shared sent copy for members, status reaches both"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    tok = ctx.uniq("S11")
    customer = f"buyer.{tok.lower()}@{EXTERNAL}"
    subject = f"售后咨询 {tok}"
    ctx.mock.inbound(from_=f"李先生 <{customer}>", to=[team.alias_email], subject=subject,
                     text="请问如何退货？")
    a_in = flows.inbox_copy(alice, subject)
    b_in = flows.inbox_copy(bob, subject)
    support = alice.identity(team.alias_email)
    assert support is not None and support["kind"] == "alias", alice.identities

    ws_bob = ctx.ws(bob.api)
    since = ctx.mark()
    draft = flows.create_draft(alice.api, mode="reply", parent_message_id=a_in["id"],
                               from_address_id=support["address_id"],
                               to=[{"name": "李先生", "email": customer}],
                               subject="Re: " + subject, html="<p>已为您安排退货</p>")
    assert draft["from_address_id"] == support["address_id"], draft
    res = flows.send_draft(alice.api, draft)

    email = flows.wait_mock_email(ctx, "Re: " + subject, since)[0]
    assert team.alias_email in email["from"] and flows.ALIAS_NAME in email["from"], email["from"]

    def shared_copy() -> dict:
        thread_id = flows.message(bob.api, b_in["id"])["thread_id"]
        outs = [m for m in flows.real_messages(flows.thread(bob.api, thread_id)) if m["direction"] == "out"]
        assert len(outs) == 1, f"bob's thread has {len(outs)} out message(s)"
        return outs[0]

    shared = eventually(shared_copy, 25, desc="shared sent copy in bob's thread")
    assert shared["from"]["email"] == team.alias_email, shared["from"]
    assert shared["sent_by"] and shared["sent_by"]["email"] == alice.email, shared["sent_by"]
    assert shared["outbound"]["id"] == res["outbound_id"], (shared["outbound"], res)
    assert shared["bcc"] == [], shared["bcc"]

    flows.wait_status(alice.api, res["message_id"], "delivered", 30)
    flows.wait_status(bob.api, shared["id"], "delivered", 30)
    ev = ws_bob.wait_for(lambda m: isinstance(m, dict) and m.get("type") == "outbound.status"
                         and m.get("outbound_id") == res["outbound_id"], 20)
    assert ev["message_id"] == shared["id"] and isinstance(ev["thread_id"], int), ev
    assert "status_detail" in ev, ev

    alice_thread = flows.message(alice.api, a_in["id"])["thread_id"]
    assert flows.message(alice.api, res["message_id"])["thread_id"] == alice_thread, \
        "alice's reply must stay in her thread"
