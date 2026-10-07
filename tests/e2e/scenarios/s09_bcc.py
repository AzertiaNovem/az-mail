"""E2E 09 — to bob, cc carol, bcc dave: BCC privacy (DESIGN C2).

dave's copy has bcc=[dave]; bob/carol have bcc=[]; alice's sent copy shows dave. The mock
exposes the full bcc list to every loopback copy (worst case), so the backend must drop it.
"""

from __future__ import annotations

from lib import flows
from lib.env import Ctx

TITLE = "BCC privacy: dave sees bcc=[self], bob/carol bcc=[], sender sees dave"
TIMEOUT = 90


def _emails(addrs: list) -> list:
    return [a["email"] for a in addrs]


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob, carol, dave = team.alice, team.bob, team.carol, team.dave
    tok = ctx.uniq("bcc").replace("-", "")
    subject = f"密送测试 {tok}"
    since = ctx.mark()
    res = flows.send(alice.api, to=[bob], cc=[carol], bcc=[dave], subject=subject, html="<p>密送</p>")

    email = flows.wait_mock_email(ctx, subject, since)[0]
    assert any(dave.email in b for b in email["bcc"]), email["bcc"]

    d = flows.inbox_copy(dave, subject)
    assert _emails(d["bcc"]) == [dave.email], d["bcc"]
    assert _emails(d["to"]) == [bob.email] and _emails(d["cc"]) == [carol.email], (d["to"], d["cc"])
    for user in (bob, carol):
        copy = flows.inbox_copy(user, subject)
        assert copy["bcc"] == [], f"{user.key} sees bcc {copy['bcc']}"
    sent = flows.message(alice.api, res["message_id"])
    assert _emails(sent["bcc"]) == [dave.email], sent["bcc"]

    recs = [r for r in ctx.mock.received(since) if r["subject"] == subject]
    assert recs and dave.email in recs[0]["received_for"], recs
    assert any(dave.email in b for b in recs[0]["bcc"]), "mock should expose bcc (worst case)"

    def ids(api, q: str) -> set:
        return {t["id"] for t in flows.list_threads(api, q=q, tzoff=480)}

    assert len(ids(alice.api, f"bcc:{dave.email} {tok}")) == 1, "sender's copy indexes BCC"
    assert ids(bob.api, f"dave {tok}") == set(), "BCC leaked into bob's search index"
    assert len(ids(dave.api, tok)) == 1
