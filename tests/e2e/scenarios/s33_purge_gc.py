"""E2E 33 — purge (retention) and blob GC remove trashed mail and its unreferenced blobs.

Retention override: instead of waiting 30 days, the test backdates ``messages.trashed_at`` and
``blobs.created_at`` in SQLite and moves the pending periodic ``purge.trash`` / ``gc.blobs`` jobs
to run now. The R2 object (mock S3) or the local blob file must disappear, then the row.
"""

from __future__ import annotations

import contextlib
import hashlib
import os

from lib import flows
from lib.env import EXTERNAL, Ctx
from lib.wait import eventually

TITLE = "purge.trash + gc.blobs remove trashed mail and its blob (R2 object / local file)"
TIMEOUT = 150
DAY_MS = 86_400_000


def run(ctx: Ctx) -> None:
    env = ctx.env
    alice = ctx.team().alice
    tok = ctx.uniq("S33")
    blob = os.urandom(8192) + tok.encode()
    sha = hashlib.sha256(blob).hexdigest()
    subject = f"待清理 {tok}"
    ctx.mock.inbound(from_=f"gc.{tok.lower()}@{EXTERNAL}", to=[alice.email], subject=subject, text="将被清理",
                     attachments=[{"filename": "purge-me.bin", "content_type": "application/octet-stream",
                                   "data": blob}])
    copy = flows.inbox_copy(alice, subject, timeout=30)
    assert copy["attachments"] and copy["attachments"][0]["size"] == len(blob), copy["attachments"]
    local_path = env.data_dir / "blobs" / sha[:2] / sha[2:4] / sha

    def stored() -> bool:
        if env.blob_backend == "r2":
            return ctx.mock.s3_object(sha) is not None
        return local_path.exists()

    eventually(lambda: _assert(stored(), "blob not stored"), 15)
    thread_id = copy["thread_id"]
    alice.api.post("/api/threads/actions", {"thread_ids": [thread_id], "action": "trash"})

    with contextlib.closing(env.db()) as db, db:
        n = db.execute("UPDATE messages SET trashed_at = trashed_at - ? WHERE thread_id = ? AND owner_id = ? "
                       "AND trashed_at IS NOT NULL", (31 * DAY_MS, thread_id, alice.id)).rowcount
        assert n >= 1, "no trashed message to backdate"
        db.execute("UPDATE jobs SET run_at = 0 WHERE kind = 'purge.trash' AND state = 'pending'")

    def purged() -> None:
        r = alice.api.call("GET", f"/api/threads/{thread_id}")
        assert r.status == 404, f"thread still there ({r.status})"

    eventually(purged, 60, 1.0, "purge.trash removed the thread")
    assert stored(), "blob must survive until gc.blobs runs"

    with contextlib.closing(env.db()) as db, db:
        db.execute("UPDATE blobs SET created_at = created_at - ? WHERE sha256 = ?", (2 * DAY_MS, sha))
        db.execute("UPDATE jobs SET run_at = 0 WHERE kind = 'gc.blobs' AND state = 'pending'")

    eventually(lambda: _assert(not stored(), "blob still stored"), 60, 1.0, "gc.blobs removed the blob")
    with contextlib.closing(env.db()) as db:
        rows = db.execute("SELECT count(*) FROM blobs WHERE sha256 = ?", (sha,)).fetchone()[0]
    assert rows == 0, "blobs row kept after the object was removed"


def _assert(cond: bool, msg: str) -> None:
    assert cond, msg
