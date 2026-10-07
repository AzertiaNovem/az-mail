"""E2E 18 — delivery events: bounce/fail/delay/complain/suppress with shuffled order and duplicate
svix-ids → final statuses by precedence (DESIGN B3); duplicates ignored."""

from __future__ import annotations

from collections import Counter

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "events: bounce/fail/delay/complain/suppress, shuffled + duplicated webhooks"
TIMEOUT = 150

CASES = {
    "bounce": ("bounced", {"email.sent", "email.bounced"}),
    "fail": ("failed", {"email.failed"}),
    "delay": ("delivered", {"email.sent", "email.delivery_delayed", "email.delivered"}),
    "complain": ("complained", {"email.sent", "email.delivered", "email.complained"}),
    "suppress": ("suppressed", {"email.sent", "email.suppressed"}),
    "ok": ("delivered", {"email.sent", "email.delivered"}),
}


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    ctx.mock.config(shuffle_events=True, duplicate_webhooks=True)
    tok = ctx.uniq("S18")
    since = ctx.mark()
    sends = {}
    for prefix in CASES:
        rcpt = f"{prefix}.{tok.lower()}@{EXTERNAL}"
        sends[prefix] = flows.send(alice.api, to=[rcpt], subject=f"事件 {prefix} {tok}",
                                   html=f"<p>{prefix}</p>")

    for prefix, (final, types) in CASES.items():
        mid = sends[prefix]["message_id"]
        msg = flows.wait_status(alice.api, mid, final, 45)
        if final in ("bounced", "failed", "suppressed"):
            assert msg["outbound"]["status_detail"], f"{prefix}: bounce/failure text expected"

        def all_events(mid: int = mid, types: set = types, prefix: str = prefix) -> list:
            events = alice.api.get(f"/api/messages/{mid}/events")["events"]
            got = {e["type"] for e in events}
            assert types <= got, f"{prefix}: events {sorted(got)} lack {sorted(types - got)}"
            return events

        events = eventually(all_events, 20)
        dup = [t for t, n in Counter(e["type"] for e in events if e["type"].startswith("email.")).items() if n > 1]
        assert not dup, f"{prefix}: duplicate webhook applied twice: {dup}"

    # the mock really delivered every webhook twice with the same svix-id
    emails = [e for e in ctx.mock.emails(since) if tok in e["subject"]]
    assert len(emails) == len(CASES), [e["subject"] for e in emails]
    deliveries = ctx.mock.webhooks(wait=True)["deliveries"]
    for email in emails:
        per = Counter(d["svix_id"] for d in deliveries if d["email_id"] == email["id"] and d["status"] == 200)
        assert per and all(n >= 2 for n in per.values()), (email["subject"], per)

    # admin event log: one row per svix-id, results recorded
    rows = ctx.admin.get("/api/admin/events")
    assert set(rows) >= {"items", "next_cursor"}, rows
    ids = Counter(r["svix_id"] for r in rows["items"])
    assert all(n == 1 for n in ids.values()), "duplicate svix-ids stored twice"
    assert all("payload" not in r for r in rows["items"]), "admins never see webhook payloads"
