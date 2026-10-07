"""E2E 21 — daily_quota_exceeded → failed, not retried; the retry endpoint works after the fault
clears (new outbound uuid); admin stats show quota_blocked (DESIGN B6)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "daily quota: failed without auto-retry; /retry after the fault clears"
TIMEOUT = 120


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    ctx.mock.faults([{"match": "POST /emails", "status": 429, "name": "daily_quota_exceeded",
                      "message": "You have reached your daily email sending quota.", "count": None}])
    tok = ctx.uniq("S21")
    subject = f"配额测试 {tok}"
    since = ctx.mark()
    res = flows.send(alice.api, to=[f"ok.{tok.lower()}@{EXTERNAL}"], subject=subject)
    failed = flows.wait_status(alice.api, res["message_id"], "failed", 40)
    assert "配额" in (failed["outbound"]["status_detail"] or ""), failed["outbound"]
    n = len(ctx.mock.posts(since, subject=subject))
    assert n == 1, f"{n} POSTs: quota errors must not be retried"
    time.sleep(6)
    assert len(ctx.mock.posts(since, subject=subject)) == 1, "quota failure was retried"
    assert ctx.admin.get("/api/admin/stats")["quota_blocked"] is True

    failed_rows = ctx.admin.get("/api/admin/outbox", query={"status": "failed"})
    assert isinstance(failed_rows, list) and any(r["id"] == res["outbound_id"] for r in failed_rows), failed_rows

    ctx.mock.clear_faults()
    retry = alice.api.post(f"/api/messages/{res['message_id']}/retry", expect=202)
    assert retry["outbound_id"] != res["outbound_id"], "retry must create a new outbound (new uuid)"
    flows.wait_status(alice.api, retry["message_id"], "delivered", 40)
    posts = ctx.mock.posts(since, subject=subject)
    assert [p["status"] for p in posts] == [429, 200], [p["status"] for p in posts]
    assert posts[0]["idempotency_key"] != posts[1]["idempotency_key"], "retry reused the old key"
    eventually(lambda: _assert_unblocked(ctx), 20)
    r = alice.api.call("POST", f"/api/messages/{retry['message_id']}/retry")
    assert r.status == 409 and r.error_code == "invalid_state", r.short()


def _assert_unblocked(ctx: Ctx) -> None:
    assert ctx.admin.get("/api/admin/stats")["quota_blocked"] is False, "quota_blocked not cleared"
