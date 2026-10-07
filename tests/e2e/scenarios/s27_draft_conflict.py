"""E2E 27 — draft version conflict: 409 version_conflict with details.current (full Draft);
``force`` overwrites; send with a stale version → 409 too (DESIGN C10, Addendum B)."""

from __future__ import annotations

from lib import flows
from lib.env import Ctx

TITLE = "draft optimistic concurrency: 409 with current draft, force, stale send"

DRAFT_KEYS = {"id", "thread_id", "version", "mode", "parent_message_id", "from_address_id", "to", "cc",
              "bcc", "subject", "html", "quoted_html", "attachments", "updated_at"}


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    api = alice.api
    subject = f"草稿冲突 {ctx.uniq('S27')}"
    draft = flows.create_draft(api, to=[bob], subject=subject, html="<p>v1</p>")
    assert DRAFT_KEYS <= set(draft), sorted(DRAFT_KEYS - set(draft))
    v1 = draft["version"]

    def body(sub: str, html: str, version: int, **extra) -> dict:
        b = flows.draft_body(to=[bob], subject=sub, html=html)
        b.update(version=version, **extra)
        return b

    d2 = api.put(f"/api/drafts/{draft['id']}", body(subject + " v2", "<p>v2</p>", v1))
    assert d2["version"] > v1 and d2["subject"] == subject + " v2", d2

    r = api.call("PUT", f"/api/drafts/{draft['id']}", body(subject + " v3", "<p>v3</p>", v1))
    assert r.status == 409 and r.error_code == "version_conflict", r.short()
    current = r.error_details.get("current")
    assert isinstance(current, dict) and DRAFT_KEYS <= set(current), current
    assert current["version"] == d2["version"] and current["subject"] == subject + " v2", current

    forced = api.put(f"/api/drafts/{draft['id']}", body(subject + " v3", "<p>v3</p>", v1, force=True))
    assert forced["subject"] == subject + " v3" and forced["version"] > d2["version"], forced
    assert api.get(f"/api/drafts/{draft['id']}")["subject"] == subject + " v3"

    r = flows.send_draft(api, {"id": draft["id"], "version": v1}, expect=None)
    assert r.status == 409 and r.error_code == "version_conflict", r.short()
    assert r.error_details["current"]["version"] == forced["version"], r.error_details

    r = api.delete(f"/api/drafts/{draft['id']}")
    assert r.body == b""
    assert api.call("GET", f"/api/drafts/{draft['id']}").status == 404
    assert not flows.threads_with_subject(api, subject + " v3", "drafts")
