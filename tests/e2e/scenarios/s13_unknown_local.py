"""E2E 13 — send to an unknown local address → 422 unknown_local_recipient (DESIGN C4).

Also the recipient caps (C12: 50 per field → 422 too_many_recipients {field}) and no recipients.
"""

from __future__ import annotations

from lib import flows
from lib.env import DOMAIN, EXTERNAL, Ctx
from lib.wait import never

TITLE = "422 unknown_local_recipient / too_many_recipients / no_recipients; draft kept"


def _send_expect(api, code: str, **fields) -> dict:
    r = api.call("POST", "/api/drafts", flows.draft_body(**fields))
    if r.status == 201:
        draft = r.json()
        r = flows.send_draft(api, draft, expect=None)
        assert r.status == 422 and r.error_code == code, r.short()
        assert api.get(f"/api/drafts/{draft['id']}")["id"] == draft["id"], "draft must survive a 422"
    else:
        assert r.status == 422 and r.error_code == code, r.short()
    return r.error_details


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    tok = ctx.uniq("S13").lower()
    nobody = f"nobody.{tok}@{DOMAIN}"
    since = ctx.mark()

    details = _send_expect(alice.api, "unknown_local_recipient", to=[nobody], subject="x")
    assert details.get("emails") == [nobody], details
    details = _send_expect(alice.api, "unknown_local_recipient", to=[bob], cc=[nobody.upper()],
                           subject="x")
    assert [e.lower() for e in details.get("emails", [])] == [nobody], details

    many = [f"r{i}.{tok}@{EXTERNAL}" for i in range(51)]
    details = _send_expect(alice.api, "too_many_recipients", to=many, subject="x")
    assert details.get("field") == "to", details
    details = _send_expect(alice.api, "too_many_recipients", to=[bob], cc=many, subject="x")
    assert details.get("field") == "cc", details
    _send_expect(alice.api, "no_recipients", subject="x")

    never(lambda: ctx.mock.requests(since, method="POST", path="/emails"), 2.0,
          desc="a rejected send reaching the mock")
