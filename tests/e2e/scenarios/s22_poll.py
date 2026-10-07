"""E2E 22 — webhooks off + inject + /admin/sync → mail appears; poll + webhook for the same id
→ one copy (DESIGN B8/B10, C8)."""

from __future__ import annotations

import time

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually, never

TITLE = "poller: webhooks off + admin sync delivers; webhook + poll for one id → 1 copy"
TIMEOUT = 120


def _single_copy(api, subject: str) -> None:
    threads = flows.threads_with_subject(api, subject, "all")
    assert len(threads) == 1, f"{len(threads)} threads"
    msgs = [m for m in flows.real_messages(flows.thread(api, threads[0]["id"])) if m["subject"] == subject]
    assert len(msgs) == 1, f"{len(msgs)} copies of {subject!r}"


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    tok = ctx.uniq("S22")
    sender = f"poll.{tok.lower()}@{EXTERNAL}"
    ctx.mock.config(webhooks_enabled=False)
    subject = f"轮询 {tok}"
    inj = ctx.mock.inbound(from_=sender, to=[alice.email], subject=subject, text="没有 webhook")
    never(lambda: flows.threads_with_subject(alice.api, subject, "all"), 3.0,
          desc="delivery without webhook or poll")
    job = ctx.admin.post("/api/admin/sync", expect=202)
    assert isinstance(job["job_id"], int), job
    flows.wait_thread(alice.api, subject, "inbox", 30)

    ctx.mock.config(webhooks_enabled=True)
    ctx.mock.webhook(email_id=inj["ids"][0])  # a late email.received for the same message
    ctx.admin.post("/api/admin/sync", expect=202)
    time.sleep(4)
    _single_copy(alice.api, subject)

    # webhook and poll racing for one id
    subject2 = f"轮询竞争 {tok}"
    inj2 = ctx.mock.inbound(from_=sender, to=[alice.email], subject=subject2, text="webhook + poll")
    ctx.admin.post("/api/admin/sync", expect=202)
    flows.wait_thread(alice.api, subject2, "inbox", 30)
    time.sleep(3)
    eventually(lambda: _single_copy(alice.api, subject2), 10)
    assert inj2["ids"], inj2
