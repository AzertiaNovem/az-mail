"""E2E 31 — unroutable inbound (unknown local recipient) is visible in admin, delivered nowhere."""

from __future__ import annotations

import time

from lib import flows
from lib.env import DOMAIN, EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "unroutable inbound shows in /api/admin/inbound?state=unroutable"


def run(ctx: Ctx) -> None:
    team = ctx.team()
    tok = ctx.uniq("S31")
    ghost = f"ghost.{tok.lower()}@{DOMAIN}"
    sender = f"stranger.{tok.lower()}@{EXTERNAL}"
    subject = f"无人认领 {tok}"
    inj = ctx.mock.inbound(from_=f"Stranger <{sender}>", to=[ghost], subject=subject, text="hello?")

    def row() -> dict:
        rows = ctx.admin.get("/api/admin/inbound", query={"state": "unroutable"})
        assert isinstance(rows, list), rows
        mine = [r for r in rows if r["resend_id"] == inj["ids"][0]]
        assert mine, "not listed as unroutable yet"
        return mine[0]

    r = eventually(row, 30, desc="unroutable inbound row")
    assert r["state"] == "unroutable", r
    assert isinstance(r["recipients"], list) and ghost in [x.lower() for x in r["recipients"]], r
    assert r["subject"] == subject, r
    assert (r["from_email"] or "").lower() == sender, r
    assert r["source"] in ("webhook", "poll"), r
    assert r["message_id_header"] and inj["message_id"].strip("<>") in r["message_id_header"], r
    time.sleep(1)
    for user in team.users.values():
        assert not flows.threads_with_subject(user.api, subject, "all"), f"{user.key} got unroutable mail"
