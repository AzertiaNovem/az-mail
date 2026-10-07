"""E2E 20 — faults: 429 ×3, 500 ×2, one timeout → exactly 1 email at the mock; the 429s do not
consume job attempts (DESIGN A6/B6).

The timeout fault lets the mock create the email but holds the response longer than
RESEND_TIMEOUT_SEC (4 s in the E2E env), so the retry must be answered by the idempotent replay
(or skipped when a webhook already recorded the send, CONTRACTS §H 31) — never a second email.
Backoff 5 s + 15 s + 1 min makes this scenario take ~1.5 min.
"""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "faults 429×3 / 500×2 / timeout: exactly one email, 429 does not count as an attempt"
TIMEOUT = 300


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    ctx.mock.faults([
        {"match": "POST /emails", "status": 429, "name": "rate_limit_exceeded", "retry_after": 1, "count": 3},
        {"match": "POST /emails", "status": 500, "name": "internal_server_error", "count": 2},
        {"match": "POST /emails", "timeout": 8, "count": 1},
    ])
    tok = ctx.uniq("S20")
    subject = f"故障注入 {tok}"
    since = ctx.mark()
    res = flows.send(alice.api, to=[f"ok.{tok.lower()}@{EXTERNAL}"], subject=subject)

    flows.wait_status(alice.api, res["message_id"], {"sent", "delivered"}, 240)

    def job_done() -> dict:
        rows = ctx.admin.get("/api/admin/jobs", query={"state": "done"})
        mine = [j for j in rows if j["kind"] == "outbound.send"
                and j["payload"].get("outbound_id") == res["outbound_id"]]
        assert mine, "outbound.send job not done yet"
        return mine[0]

    job = eventually(job_done, 120, 1.0, "outbound.send job finished")
    posts = ctx.mock.posts(since, subject=subject)
    statuses = [p["status"] for p in posts]
    assert statuses[:6] == [429, 429, 429, 500, 500, 200], statuses
    assert all(p["status"] == 200 and p["replayed"] for p in posts[6:]), \
        [(p["status"], p["replayed"]) for p in posts[6:]]
    assert len({p["idempotency_key"] for p in posts}) == 1, "every retry must reuse the Idempotency-Key"
    assert len(ctx.mock.emails(since, subject)) == 1, "the timeout retry created a second email"
    # attempts: 500, 500, timeout, final → 4 (the three 429s were given back)
    assert job["attempts"] == 4, f"attempts={job['attempts']} (429s must not count): {job}"
    assert job["state"] == "done" and isinstance(job["payload"], dict), job
