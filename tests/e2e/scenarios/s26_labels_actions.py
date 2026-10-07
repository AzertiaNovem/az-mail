"""E2E 26 — labels CRUD + thread actions (archive/star/read/trash/restore/spam/delete_forever);
counts and folders stay correct."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx

TITLE = "labels CRUD and every thread action with counts/folders"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    api = alice.api
    tok = ctx.uniq("S26")

    label = api.post("/api/labels", {"name": f"重要-{tok}", "color": "#d93025"}, expect=201)
    assert set(label) >= {"id", "name", "color", "sort_order"} and label["color"] == "#d93025", label
    r = api.call("POST", "/api/labels", {"name": f"重要-{tok}".upper(), "color": "#000000"})
    assert r.status == 409 and r.error_code == "label_exists", r.short()
    assert any(l["id"] == label["id"] for l in api.get("/api/labels")), "GET /api/labels is a bare array"
    renamed = api.patch(f"/api/labels/{label['id']}", {"name": f"很重要-{tok}"})
    assert renamed["name"] == f"很重要-{tok}" and renamed["color"] == "#d93025", renamed
    assert api.get(f"/api/labels/{label['id']}") == renamed

    subject = f"标签与操作 {tok}"
    ctx.mock.inbound(from_=f"ops.{tok.lower()}@{EXTERNAL}", to=[alice.email], subject=subject, text="操作测试")
    item = flows.wait_thread(api, subject, "inbox")
    t = item["id"]
    base = flows.counts(api)

    def act(action: str, **extra) -> None:
        out = api.post("/api/threads/actions", {"thread_ids": [t], "action": action, **extra})
        assert out == {"thread_ids": [t]}, (action, out)

    def in_folder(folder: str) -> bool:
        return any(x["id"] == t for x in flows.list_threads(api, folder=folder))

    act("add_label", label_id=label["id"])
    assert label["id"] in flows.thread(api, t)["label_ids"]
    assert any(x["id"] == t for x in flows.list_threads(api, folder=None, label_id=label["id"]))
    lc = flows.counts(api)["labels"][str(label["id"])]
    assert lc == {"unread": 1, "total": 1}, lc

    act("read")
    assert flows.counts(api)["inbox_unread"] == base["inbox_unread"] - 1
    assert flows.counts(api)["labels"][str(label["id"])]["unread"] == 0
    act("unread")
    assert flows.counts(api)["inbox_unread"] == base["inbox_unread"]
    act("star")
    assert in_folder("starred") and flows.thread(api, t)["messages"][0]["is_starred"] is True
    act("unstar")
    assert not in_folder("starred")
    act("archive")
    assert not in_folder("inbox") and in_folder("all")
    assert flows.counts(api)["inbox_unread"] == base["inbox_unread"] - 1
    act("inbox")
    assert in_folder("inbox")
    act("trash")
    assert in_folder("trash") and not in_folder("inbox") and not in_folder("all")
    assert flows.thread(api, t)["messages"][0]["trashed"] is True
    act("restore")
    assert in_folder("inbox") and not in_folder("trash")
    act("spam")
    assert in_folder("spam") and not in_folder("inbox")
    act("not_spam")
    assert in_folder("inbox") and not in_folder("spam")
    act("remove_label", label_id=label["id"])
    assert label["id"] not in flows.thread(api, t)["label_ids"]

    act("add_label", label_id=label["id"])
    r = api.delete(f"/api/labels/{label['id']}")
    assert r.body == b"", "DELETE /api/labels/:id returns 204 without body"
    assert label["id"] not in flows.thread(api, t)["label_ids"], "deleted label still attached"
    assert api.call("GET", f"/api/labels/{label['id']}").status == 404

    r = api.call("POST", "/api/threads/actions", {"thread_ids": [t], "action": "explode"})
    assert r.status == 400, r.short()

    act("trash")
    act("delete_forever")
    r = api.call("GET", f"/api/threads/{t}")
    assert r.status == 404 and r.error_code == "not_found", r.short()
    assert not in_folder("trash")
