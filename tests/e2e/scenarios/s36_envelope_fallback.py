"""E2E 36 — envelope fallback (DESIGN C1, F4.1): Resend documents ``received_for`` as the
addresses of the Received headers' FOR clauses, and MTAs leave that clause out when one SMTP
transaction has several recipients. The rest of the suite runs with the mock's idealised full
envelope; here the mock omits the field, derives it from its own Received trace header (empty for
a multi-recipient delivery) or splits the delivery per recipient, so the To/Cc fallback and the
per-transaction FOR clause are both exercised against the real backend.

Not asserted (unverified Resend behaviour, see tools/mock_resend/README.md "Envelope"): with an
empty envelope a Bcc-only local recipient cannot be found, and with a partial envelope
(received_for_mode "first") the unnamed local recipients get no copy.
"""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually, never

TITLE = "received_for missing / empty / per-transaction FOR clause: routing falls back to To/Cc"
TIMEOUT = 150


def _copies(user, subject: str) -> list:
    out = []
    for t in flows.threads_with_subject(user.api, subject, "all"):
        out += [m for m in flows.real_messages(flows.thread(user.api, t["id"])) if m["subject"] == subject]
    return out


def _one_copy(user, subject: str) -> dict:
    def check() -> dict:
        msgs = _copies(user, subject)
        assert len(msgs) == 1, f"{user.key}: {len(msgs)} copies of {subject!r}"
        return msgs[0]

    flows.wait_thread(user.api, subject, "inbox", 30)
    return eventually(check, 15, 0.4, f"{user.key} has one copy")


def _received(ctx: Ctx, since: int, subject: str, count: int = 1) -> list:
    def check() -> list:
        recs = [r for r in ctx.mock.received(since) if r["subject"] == subject]
        assert len(recs) == count, f"mock stored {len(recs)} inbound email(s) {subject!r}, want {count}"
        return recs

    return eventually(check, 20, 0.3, f"mock inbound {subject!r}")


def _nothing_for(users, subject: str) -> None:
    never(lambda: any(_copies(u, subject) for u in users), 2.0,
          desc=f"{'/'.join(u.key for u in users)} receiving {subject!r}")


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob, carol, dave = team.alice, team.bob, team.carol, team.dave
    tok = ctx.uniq("S36")

    # 1. received_for absent from the webhook and GET /emails/receiving/{id}: external mail
    #    To alice, Cc support@ (alice + bob) → routed by the headers; one copy per owner.
    ctx.mock.config(received_for_mode="omit")
    subject = f"无信封字段 {tok}"
    customer = f"no.envelope.{tok.lower()}@{EXTERNAL}"
    since = ctx.mark()
    inj = ctx.mock.inbound(from_=f"王客户 <{customer}>", to=[alice.email], cc=[team.alias_email],
                           subject=subject, text="received_for 缺失")
    assert inj["received_for"] == [None], inj
    rec = _received(ctx, since, subject)[0]
    assert rec["received_for"] is None, rec["received_for"]
    a = _one_copy(alice, subject)
    b = _one_copy(bob, subject)
    assert b["delivered_to"] == team.alias_email, b["delivered_to"]
    assert a["delivered_to"] in (alice.email, team.alias_email), a["delivered_to"]
    for copy in (a, b):
        assert copy["from"]["email"] == customer, copy["from"]
        assert copy["in_inbox"] is True and copy["is_read"] is False, copy
    _nothing_for((carol, dave), subject)

    # 2. "received": received_for comes from the MX's Received FOR clause. Internal mail
    #    carol → To alice, bob; Cc dave is ONE transaction with three recipients, so there is no
    #    FOR clause and received_for == [] → To/Cc fallback; the loopback is still recognised
    #    (carol keeps exactly her sent copy, no inbound duplicate).
    ctx.mock.config(received_for_mode="received")
    subject2 = f"多收件人回环 {tok}"
    since = ctx.mark()
    res = flows.send(carol.api, to=[alice, bob], cc=[dave], subject=subject2, html="<p>内部群发</p>")
    rec = _received(ctx, since, subject2)[0]
    assert rec["source"] == "loopback" and rec["received_for"] == [], rec["received_for"]
    assert "received" in rec["headers"] and " for " not in rec["headers"]["received"], rec["headers"]
    for user in (alice, bob, dave):
        copy = _one_copy(user, subject2)
        assert copy["delivered_to"] == user.email, (user.key, copy["delivered_to"])
        assert copy["from"]["email"] == carol.email, copy["from"]
    flows.wait_status(carol.api, res["message_id"], "delivered")
    mine = _copies(carol, subject2)
    assert [m["id"] for m in mine] == [res["message_id"]], f"carol: {[(m['id'], m['in_inbox']) for m in mine]}"

    # 3. "received" + split delivery: one transaction per envelope recipient, each with its FOR
    #    clause → received_for [alice] / [dave]. The Bcc-only recipient dave is reached through the
    #    envelope (never through headers), and nobody named only in a header gets a copy.
    ctx.mock.config(received_for_mode="received", split_delivery=True)
    subject3 = f"逐个投递密送 {tok}"
    since = ctx.mark()
    inj = ctx.mock.inbound(from_=f"partner.{tok.lower()}@{EXTERNAL}", to=[alice.email],
                           bcc=[dave.email], subject=subject3, text="每个收件人一次投递")
    assert sorted(r[0] for r in inj["received_for"]) == sorted([alice.email, dave.email]), inj
    recs = _received(ctx, since, subject3, 2)
    assert {r["message_id"] for r in recs} == {inj["message_id"]}, recs
    assert _one_copy(alice, subject3)["delivered_to"] == alice.email
    d = _one_copy(dave, subject3)
    assert d["delivered_to"] == dave.email, d["delivered_to"]
    assert [x["email"] for x in d["to"]] == [alice.email], d["to"]
    _nothing_for((bob, carol), subject3)
