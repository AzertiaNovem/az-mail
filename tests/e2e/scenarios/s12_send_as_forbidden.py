"""E2E 12 — bob (member without can_send_as) tries to send as support@ → 403 send_as_forbidden."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import never

TITLE = "send-as enforcement: 403 send_as_forbidden, nothing reaches Resend"


def _attempt(api, from_address_id: int, subject: str) -> None:
    body = flows.draft_body(to=[f"someone@{EXTERNAL}"], subject=subject, html="<p>x</p>",
                            from_address_id=from_address_id)
    r = api.call("POST", "/api/drafts", body)
    if r.status == 201:
        r = flows.send_draft(api, r.json(), expect=None)
    assert r.status == 403 and r.error_code == "send_as_forbidden", r.short()


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    support = alice.identity(team.alias_email)
    assert support is not None
    tok = ctx.uniq("S12")
    since = ctx.mark()
    _attempt(bob.api, support["address_id"], f"冒充客服 {tok}")
    _attempt(bob.api, alice.address_id, f"冒充同事 {tok}")
    never(lambda: [p for p in ctx.mock.requests(since, method="POST", path="/emails")], 2.0,
          desc="a forbidden send reaching the mock")
