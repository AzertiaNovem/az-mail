"""E2E 29 — IDOR: bob GETs/changes alice's thread/message/attachment/draft/label → 404 everywhere
(DESIGN D6, CONTRACTS §G 1)."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx

TITLE = "IDOR: every foreign id behaves like a missing one (404)"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    tok = ctx.uniq("S29")
    att = flows.upload(alice.api, "机密.txt", "text/plain", b"top secret")
    res = flows.send(alice.api, to=[f"ok.{tok.lower()}@{EXTERNAL}"], subject=f"私密 {tok}",
                     attachment_ids=[att["id"]])
    ctx.mock.inbound(from_=f"in.{tok.lower()}@{EXTERNAL}", to=[alice.email], subject=f"私密来信 {tok}", text="x")
    inbound = flows.inbox_copy(alice, f"私密来信 {tok}")
    draft = flows.create_draft(alice.api, to=[bob], subject=f"私密草稿 {tok}")
    label = alice.api.post("/api/labels", {"name": f"私密-{tok}", "color": "#000000"}, expect=201)
    ctx.defer(lambda: alice.api.request("DELETE", f"/api/labels/{label['id']}"))
    mid, tid = res["message_id"], res["thread_id"]

    probes = [
        ("GET", f"/api/threads/{tid}", None),
        ("GET", f"/api/messages/{mid}", None),
        ("PATCH", f"/api/messages/{mid}", {"is_read": True}),
        ("GET", f"/api/messages/{mid}/events", None),
        ("GET", f"/api/messages/{inbound['id']}/raw", None),
        ("POST", f"/api/messages/{mid}/undo-send", None),
        ("POST", f"/api/messages/{mid}/retry", None),
        ("POST", f"/api/messages/{mid}/cancel-schedule", None),
        ("POST", f"/api/messages/{mid}/reschedule", {"scheduled_at": flows.now_ms() + 3_600_000}),
        ("GET", f"/api/drafts/{draft['id']}", None),
        ("PUT", f"/api/drafts/{draft['id']}", {**flows.draft_body(subject="hijack"), "version": draft["version"]}),
        ("POST", f"/api/drafts/{draft['id']}/send", {"version": draft["version"]}),
        ("DELETE", f"/api/drafts/{draft['id']}", None),
        ("GET", f"/api/labels/{label['id']}", None),
        ("PATCH", f"/api/labels/{label['id']}", {"name": "hijack"}),
        ("DELETE", f"/api/labels/{label['id']}", None),
    ]
    for method, path, body in probes:
        r = bob.api.call(method, path, body)
        assert r.status == 404 and r.error_code == "not_found", f"{method} {path}: {r.status} {r.short()}"

    # thread actions on foreign ids change nothing
    r = bob.api.call("POST", "/api/threads/actions", {"thread_ids": [tid], "action": "trash"})
    assert r.status in (200, 404), r.short()
    if r.status == 200:
        assert r.json()["thread_ids"] == [], r.json()
    assert flows.message(alice.api, mid)["trashed"] is False
    r = bob.api.call("POST", "/api/drafts", flows.draft_body(subject="x", attachment_ids=[att["id"]]))
    assert r.status in (404, 422, 400), f"foreign attachment id accepted: {r.status} {r.short()}"
    r = bob.api.call("POST", "/api/threads/actions", {"thread_ids": [inbound["thread_id"]],
                                                      "action": "add_label", "label_id": label["id"]})
    assert r.status in (200, 404), r.short()
    assert label["id"] not in flows.thread(alice.api, inbound["thread_id"])["label_ids"]

    # signed file URLs are bound to the user: bob cannot use alice's attachment id
    sent = flows.message(alice.api, mid)
    url = sent["attachments"][0]["download_url"]
    assert flows.fetch(url).status == 200, "alice's own URL works"
    signer = ctx.signer(url)
    if signer is not None:
        exp = int(flows.query_of(url)["exp"])
        r = flows.fetch(signer.file_url(sent["attachments"][0]["id"], bob.id, "a", exp))
        assert r.status in (403, 404), f"bob fetched alice's attachment: {r.status}"
        r = flows.fetch(signer.raw_url(inbound["id"], bob.id, exp))
        assert r.status in (403, 404), f"bob fetched alice's raw message: {r.status}"
    else:
        ctx.log("signed-URL forging unavailable; skipped the u=<bob> probe")
    r = flows.fetch(url.replace(f"u={alice.id}", f"u={bob.id}"))
    assert r.status in (403, 404), r.status

    assert alice.api.get(f"/api/drafts/{draft['id']}")["subject"] == f"私密草稿 {tok}"
    assert alice.api.get(f"/api/labels/{label['id']}")["name"] == f"私密-{tok}"
