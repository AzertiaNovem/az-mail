"""E2E 25 — search: 3+ Chinese chars (FTS trigram), 2-char term (LIKE), from:, has:attachment,
label:, in:anywhere, before/after with tzoff, negation, OR (docs/API.md search grammar)."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "search operators incl. Chinese terms, dates with tzoff, negation, OR"
TIMEOUT = 90


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    tag = ctx.uniq("srch").replace("-", "")  # one ASCII token (≥ 3 chars) scoping every query
    zhang = f"zhang.{tag}@{EXTERNAL}"
    li = f"li.{tag}@{EXTERNAL}"
    now = flows.now_ms()
    mails = {
        "A": dict(from_=f"张伟 <{zhang}>", subject=f"季度报告汇总 {tag}", text="第三季度的报告已完成",
                  date=now - 86_400_000, attachments=[{"filename": "report.xlsx",
                                                       "content_type": "application/vnd.ms-excel",
                                                       "data": b"PK\x03\x04 xlsx"}]),
        "B": dict(from_=f"李娜 <{li}>", subject=f"周报 {tag}", text="本周进度正常", date=now - 2 * 86_400_000),
        "C": dict(from_=f"张伟 <{zhang}>", subject=f"Meeting notes {tag}", text="agenda and minutes",
                  date="2026-01-10T10:00:00+08:00"),
        "D": dict(from_=f"李娜 <{li}>", subject=f"Late night {tag}", text="after midnight",
                  date="2026-03-02T01:00:00+08:00"),  # 2026-03-01 17:00 UTC
    }
    for spec in mails.values():
        ctx.mock.inbound(to=[alice.email], **spec)
    tid = {}
    for key, spec in mails.items():
        tid[key] = flows.wait_thread(alice.api, spec["subject"], "inbox", 30)["id"]

    label = alice.api.post("/api/labels", {"name": f"项目{tag}", "color": "#1a73e8"}, expect=201)
    ctx.defer(lambda: alice.api.request("DELETE", f"/api/labels/{label['id']}"))
    alice.api.post("/api/threads/actions", {"thread_ids": [tid["A"]], "action": "add_label",
                                            "label_id": label["id"]})

    def ids(q: str, tzoff: int = 480) -> set:
        page = alice.api.get("/api/threads", query={"q": q, "tzoff": tzoff, "limit": 100})
        assert page["total"] is None, f"search results have total=null: {page['total']}"
        return {t["id"] for t in page["items"]}

    def expect(q: str, keys: str, tzoff: int = 480) -> None:
        want = {tid[k] for k in keys}

        def check() -> None:
            got = ids(q, tzoff)
            names = sorted(k for k, v in tid.items() if v in got) + sorted(
                f"#{t}" for t in got if t not in tid.values())
            assert got == want, f"q={q!r} tzoff={tzoff}: got {names}, want {sorted(keys)}"

        eventually(check, 10, 0.5)

    expect(f"季度报告 {tag}", "A")          # ≥ 3 CJK chars: FTS trigram
    expect(f"周报 {tag}", "B")              # 2 chars: LIKE
    expect(f"subject:周报 {tag}", "B")
    expect(f"from:{zhang} {tag}", "AC")
    expect(f"has:attachment {tag}", "A")
    expect(f"filename:report {tag}", "A")
    expect(f"label:项目{tag}", "A")
    expect(f"{tag} -周报", "ACD")
    expect(f"{tag} 季度报告 OR 周报", "AB")
    expect(f"after:2026/02/01 {tag}", "ABD")
    expect(f"before:2026/02/01 {tag}", "C")
    expect(f"before:2026-03-02 {tag}", "C", tzoff=480)  # D is 2026-03-02 01:00 in UTC+8
    expect(f"before:2026/03/02 {tag}", "CD", tzoff=0)   # … but 2026-03-01 17:00 in UTC
    expect(f"newer_than:7d {tag}", "AB")
    expect(f"older_than:30d {tag}", "CD")

    alice.api.post("/api/threads/actions", {"thread_ids": [tid["A"]], "action": "read"})
    expect(f"is:unread {tag}", "BCD")
    expect(f"is:read {tag}", "A")
    alice.api.post("/api/threads/actions", {"thread_ids": [tid["C"]], "action": "trash"})
    expect(f"{tag}", "ABD")                 # default scope excludes trash
    expect(f"in:anywhere {tag}", "ABCD")
    expect(f"in:trash {tag}", "C")
    alice.api.post("/api/threads/actions", {"thread_ids": [tid["B"]], "action": "star"})
    expect(f"is:starred {tag}", "B")
    expect(f"in:starred {tag}", "B")
