"""Reusable E2E flows: the test team (E2E 02), composing/sending, finding threads, statuses."""

from __future__ import annotations

import secrets
import time
import urllib.parse
from dataclasses import dataclass, field
from typing import Any, Iterable

from .api import Api, Response
from .env import DOMAIN, Ctx
from .wait import eventually

TEAM_SPEC = [
    ("alice", "爱丽丝 Alice"),
    ("bob", "鲍勃 Bob"),
    ("carol", "卡罗尔 Carol"),
    ("dave", "戴夫 Dave"),
]
ALIAS_LOCAL = "support"
ALIAS_NAME = "客服支持"

FINAL_STATUSES = {"delivered", "bounced", "complained", "failed", "suppressed", "canceled"}


@dataclass
class User:
    key: str
    email: str
    name: str
    password: str
    id: int
    api: Api
    address_id: int
    identities: list[dict[str, Any]] = field(default_factory=list)

    @property
    def address(self) -> dict[str, str]:
        return {"name": self.name, "email": self.email}

    def identity(self, email: str) -> dict[str, Any] | None:
        return next((i for i in self.identities if i["email"].lower() == email.lower()), None)


@dataclass
class Team:
    users: dict[str, User]
    alias_id: int
    alias_email: str

    @property
    def alice(self) -> User:
        return self.users["alice"]

    @property
    def bob(self) -> User:
        return self.users["bob"]

    @property
    def carol(self) -> User:
        return self.users["carol"]

    @property
    def dave(self) -> User:
        return self.users["dave"]

    def by_email(self, email: str) -> User:
        return next(u for u in self.users.values() if u.email == email.lower())


def new_password() -> str:
    return "Pw-" + secrets.token_urlsafe(12) + "9a"


def ensure_team(ctx: Ctx, check: bool = False) -> Team:
    """Creates (or re-adopts) alice, bob, carol, dave and support@ (alice send-as, bob member).

    With ``check`` the API responses of fresh creations are asserted (E2E 02).
    """
    team: Team | None = ctx.state.get("team")
    if team is not None:
        for user in team.users.values():
            if user.api.request("GET", "/api/auth/me").status != 200:
                relogin(ctx, user)
        return team
    admin = ctx.admin
    existing = {u["email"].lower(): u for u in admin.get("/api/admin/users")}
    created: dict[str, dict[str, Any]] = {}
    passwords: dict[str, str] = {}
    for key, name in TEAM_SPEC:
        email = f"{key}@{DOMAIN}"
        pw = new_password()
        passwords[key] = pw
        if email in existing:
            row = admin.patch(f"/api/admin/users/{existing[email]['id']}",
                              {"password": pw, "disabled": False, "display_name": name})
        else:
            row = admin.post("/api/admin/users", {"email": email, "display_name": name, "password": pw},
                             expect=201)
            if check:
                assert row["email"] == email, row
                assert row["display_name"] == name, row
                assert row["is_admin"] is False and row["disabled"] is False, row
                assert isinstance(row["id"], int) and isinstance(row["created_at"], int), row
                assert row["last_login_at"] is None, row
                assert row["message_count"] == 0 and row["storage_bytes"] == 0, row
                assert row["aliases"] == [], row
        created[key] = row
    alias_email = f"{ALIAS_LOCAL}@{DOMAIN}"
    members = [{"user_id": created["alice"]["id"], "can_send_as": True},
               {"user_id": created["bob"]["id"], "can_send_as": False}]
    aliases = {a["email"].lower(): a for a in admin.get("/api/admin/aliases")}
    if alias_email in aliases:
        alias = admin.patch(f"/api/admin/aliases/{aliases[alias_email]['id']}",
                            {"display_name": ALIAS_NAME, "share_sent": True, "members": members})
    else:
        alias = admin.post("/api/admin/aliases", {"email": alias_email, "display_name": ALIAS_NAME,
                                                  "share_sent": True, "members": members}, expect=201)
    users: dict[str, User] = {}
    for key, name in TEAM_SPEC:
        api, login = ctx.api.login(f"{key}@{DOMAIN}", passwords[key])
        me = login["user"]
        own = next(i for i in me["identities"] if i["kind"] == "user")
        users[key] = User(key=key, email=f"{key}@{DOMAIN}", name=name, password=passwords[key],
                          id=me["id"], api=api, address_id=own["address_id"],
                          identities=me["identities"])
        api.put("/api/settings", {"undo_send_seconds": 0})
    team = Team(users=users, alias_id=alias["id"], alias_email=alias_email)
    ctx.state["team"] = team
    return team


def relogin(ctx: Ctx, user: User) -> None:
    api, login = ctx.api.login(user.email, user.password)
    user.api.token = api.token
    user.identities = login["user"]["identities"]


def set_undo(ctx: Ctx, user: User, seconds: int) -> None:
    """Sets the user's undo window for this scenario; restored to 0 afterwards."""
    got = user.api.put("/api/settings", {"undo_send_seconds": seconds})
    assert got["undo_send_seconds"] == seconds, got
    ctx.defer(lambda: user.api.request("PUT", "/api/settings", {"undo_send_seconds": 0}))


# ---------------------------------------------------------------------------------------------
# composing and sending
# ---------------------------------------------------------------------------------------------

def addr(x: Any) -> dict[str, str]:
    if isinstance(x, User):
        return x.address
    if isinstance(x, dict):
        return {"name": x.get("name", ""), "email": x["email"]}
    return {"name": "", "email": str(x)}


def draft_body(*, to: Iterable[Any] = (), cc: Iterable[Any] = (), bcc: Iterable[Any] = (),
               subject: str = "", html: str = "<p>你好</p>", **extra: Any) -> dict[str, Any]:
    body = {"to": [addr(a) for a in to], "cc": [addr(a) for a in cc], "bcc": [addr(a) for a in bcc],
            "subject": subject, "html": html}
    body.update({k: v for k, v in extra.items() if v is not None})
    return body


def create_draft(api: Api, **fields: Any) -> dict[str, Any]:
    draft = api.post("/api/drafts", draft_body(**fields), expect=201)
    assert isinstance(draft["id"], int) and isinstance(draft["version"], int), draft
    return draft


def send_draft(api: Api, draft: dict[str, Any], *, scheduled_at: int | None = None,
               final: dict[str, Any] | None = None, expect: int | None = 202) -> Any:
    body: dict[str, Any] = {"version": draft["version"]}
    if scheduled_at is not None:
        body["scheduled_at"] = scheduled_at
    if final is not None:
        body["draft"] = final
    resp = api.request("POST", f"/api/drafts/{draft['id']}/send", body, expect=expect)
    if expect is None:
        return resp
    result = resp.json()
    for key in ("message_id", "thread_id", "outbound_id", "status", "undo_ms", "scheduled_at"):
        assert key in result, f"SendResult lacks {key}: {result}"
    return result


def send(api: Api, *, scheduled_at: int | None = None, **fields: Any) -> dict[str, Any]:
    """Creates a draft and sends it (202 SendResult)."""
    return send_draft(api, create_draft(api, **fields), scheduled_at=scheduled_at)


def upload(api: Api, filename: str, content_type: str, data: bytes, inline: bool = False,
           expect: int | None = 201) -> Any:
    resp = api.request("POST", "/api/attachments", data=data,
                       headers={"Content-Type": content_type},
                       query={"filename": filename, "inline": "1" if inline else "0"}, expect=expect)
    return resp.json() if expect is not None else resp


# ---------------------------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------------------------

def list_threads(api: Api, *, folder: str | None = "all", label_id: int | None = None,
                 q: str | None = None, tzoff: int | None = None, limit: int = 100,
                 max_pages: int = 20) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    cursor = None
    for _ in range(max_pages):
        query = {"limit": limit, "cursor": cursor, "tzoff": tzoff}
        if q is not None:
            query["q"] = q
        elif label_id is not None:
            query["label_id"] = label_id
        else:
            query["folder"] = folder
        page = api.get("/api/threads", query=query)
        assert set(page) >= {"items", "next_cursor", "total"}, page
        items += page["items"]
        cursor = page["next_cursor"]
        if not cursor:
            break
    return items


def threads_with_subject(api: Api, subject: str, folder: str = "all") -> list[dict[str, Any]]:
    return [t for t in list_threads(api, folder=folder) if t["subject"] == subject]


def wait_thread(api: Api, subject: str, folder: str = "inbox", timeout: float = 25.0) -> dict[str, Any]:
    def check() -> dict[str, Any]:
        found = threads_with_subject(api, subject, folder)
        assert found, f"no thread {subject!r} in {folder}"
        return found[0]

    return eventually(check, timeout, 0.4, f"thread {subject!r} in {folder}")


def thread(api: Api, thread_id: int) -> dict[str, Any]:
    return api.get(f"/api/threads/{thread_id}")


def message(api: Api, message_id: int) -> dict[str, Any]:
    return api.get(f"/api/messages/{message_id}")


def real_messages(detail: dict[str, Any]) -> list[dict[str, Any]]:
    """Non-draft, non-trashed messages of a ThreadDetail (the UI filters the same way)."""
    return [m for m in detail["messages"] if not m["is_draft"] and not m["trashed"]]


def wait_status(api: Api, message_id: int, wanted: str | Iterable[str],
                timeout: float = 30.0) -> dict[str, Any]:
    want = {wanted} if isinstance(wanted, str) else set(wanted)

    def check() -> dict[str, Any]:
        msg = message(api, message_id)
        assert msg["outbound"] is not None, f"message {message_id} has no outbound"
        status = msg["outbound"]["status"]
        assert status in want, f"status {status!r} (detail {msg['outbound']['status_detail']!r}), want {sorted(want)}"
        return msg

    return eventually(check, timeout, 0.4, f"outbound status of message {message_id}")


def wait_mock_email(ctx: Ctx, subject: str, since: int = 0, timeout: float = 25.0,
                    count: int = 1) -> list[dict[str, Any]]:
    def check() -> list[dict[str, Any]]:
        found = ctx.mock.emails(since=since, subject=subject)
        assert len(found) >= count, f"mock has {len(found)} email(s) with subject {subject!r}, want {count}"
        return found

    return eventually(check, timeout, 0.3, f"mock email {subject!r}")


def inbox_copy(user: User, subject: str, folder: str = "inbox", timeout: float = 25.0) -> dict[str, Any]:
    """The single non-draft message with ``subject`` in ``user``'s thread from ``folder``."""
    item = wait_thread(user.api, subject, folder, timeout)
    msgs = [m for m in real_messages(thread(user.api, item["id"])) if m["subject"] == subject]
    assert msgs, f"{user.key}: thread {item['id']} has no message {subject!r}"
    return msgs[-1]


def counts(api: Api) -> dict[str, Any]:
    return api.get("/api/counts")


def now_ms() -> int:
    return int(time.time() * 1000)


def query_of(url: str) -> dict[str, str]:
    return dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(url).query, keep_blank_values=True))


def fetch(url: str, headers: dict[str, str] | None = None) -> Response:
    from .api import http_request

    return http_request("GET", url, headers=headers)
