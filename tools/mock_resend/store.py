"""In-memory state of the mock (thread-safe through one re-entrant lock).

Two clocks:
* ``real()`` — wall clock. Used for Svix timestamps, download-URL expiry, S3 auth and the rate
  limiter (the backend verifies these against its own clock).
* ``now()`` — the *mock* clock = wall clock + ``offset``. ``POST /_mock/advance`` moves it forward
  so scheduled sends fire without waiting; created_at / scheduled_at use it.
"""

from __future__ import annotations

import fnmatch
import hashlib
import itertools
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any

DEFAULT_CONFIG: dict[str, Any] = {
    "webhooks_enabled": True,
    "webhook_url": "",
    "shuffle_events": False,
    "duplicate_webhooks": False,
    "split_delivery": False,
    "strip_custom_headers": False,   # drop X-* request headers from loopback copies
    "strip_thread_headers": False,   # also drop In-Reply-To / References from loopback copies
    "meta_delay": 0.0,               # seconds until GET /emails/{id}.message_id is non-null
    "raw_missing": False,            # GET receiving/{id} returns "raw": null
    "reject_scheduled_attachments": False,
    "honor_message_id": False,       # use a request "Message-ID" header (F4.5: unverified)
    "verify_from_domain": True,      # from must be on a verified (local) domain
    "local_domains": [],
    "rate_limit": 10,                # requests per second (fixed window); 0 = off
    "download_ttl": 3600,            # seconds a /_dl URL stays valid
    "event_spacing": 0.15,           # seconds between consecutive events of one email
    "delivery_delay": 0.2,           # seconds from accept/fire to the first delivery event
}


@dataclass
class Attachment:
    id: str
    filename: str
    content_type: str
    data: bytes
    content_id: str | None = None
    disposition: str = "attachment"  # "inline" | "attachment"

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    def meta(self) -> dict[str, Any]:
        return {"id": self.id, "filename": self.filename, "content_type": self.content_type,
                "content_id": self.content_id, "content_disposition": self.disposition,
                "size": self.size, "sha256": self.sha256}


@dataclass
class SentEmail:
    id: str
    created_at: float  # mock clock
    from_: str
    to: list[str]
    cc: list[str]
    bcc: list[str]
    reply_to: list[str]
    subject: str
    html: str | None
    text: str | None
    headers: list[tuple[str, str]]
    attachments: list[Attachment]
    tags: dict[str, str]
    scheduled_at: float | None
    message_id: str
    message_id_at: float | None  # real time when message_id becomes visible (None = not yet)
    last_event: str
    idempotency_key: str | None
    user_agent: str
    seq: int
    events: list[dict[str, Any]] = field(default_factory=list)  # emitted (type, at, svix_id)
    dispatched: bool = False  # delivery simulation started (immediately, or when the schedule fired)

    def recipients(self) -> list[str]:
        return list(self.to) + list(self.cc) + list(self.bcc)

    def message_id_visible(self, real_now: float) -> str | None:
        if self.message_id_at is None or real_now < self.message_id_at:
            return None
        return self.message_id

    def summary(self) -> dict[str, Any]:
        return {
            "id": self.id, "seq": self.seq, "created_at": self.created_at, "from": self.from_,
            "to": self.to, "cc": self.cc, "bcc": self.bcc, "reply_to": self.reply_to,
            "subject": self.subject, "html": self.html, "text": self.text,
            "headers": dict(self.headers), "tags": self.tags,
            "attachments": [a.meta() for a in self.attachments],
            "scheduled_at": self.scheduled_at, "message_id": self.message_id,
            "message_id_at": self.message_id_at, "last_event": self.last_event,
            "idempotency_key": self.idempotency_key, "user_agent": self.user_agent,
            "events": list(self.events), "dispatched": self.dispatched,
        }


@dataclass
class ReceivedEmail:
    id: str
    created_at: float  # mock clock
    from_: str
    to: list[str]
    cc: list[str]
    bcc: list[str]
    reply_to: list[str]
    received_for: list[str]
    subject: str
    message_id: str
    html: str | None
    text: str | None
    headers: dict[str, str]
    authentication: dict[str, str] | None
    raw: bytes
    attachments: list[Attachment]
    source: str  # "loopback" | "inject"
    sent_email_id: str | None
    seq: int

    def summary(self, with_raw: bool = False) -> dict[str, Any]:
        out = {
            "id": self.id, "seq": self.seq, "created_at": self.created_at, "from": self.from_,
            "to": self.to, "cc": self.cc, "bcc": self.bcc, "reply_to": self.reply_to,
            "received_for": self.received_for, "subject": self.subject,
            "message_id": self.message_id, "html": self.html, "text": self.text,
            "headers": self.headers, "authentication": self.authentication,
            "attachments": [a.meta() for a in self.attachments], "source": self.source,
            "sent_email_id": self.sent_email_id, "raw_size": len(self.raw),
            "raw_sha256": hashlib.sha256(self.raw).hexdigest(),
        }
        if with_raw:
            out["raw"] = self.raw.decode("utf-8", "replace")
        return out


@dataclass
class IdemEntry:
    body_hash: str
    created: float  # mock clock
    state: str  # "in_flight" | "done"
    status: int = 0
    body: bytes = b""


@dataclass
class Fault:
    match: str  # fnmatch pattern over "METHOD /path" (path without query)
    target: str = "resend"  # "resend" | "s3"
    status: int | None = None
    name: str | None = None
    message: str | None = None
    retry_after: int | None = None
    body: str | None = None  # raw (non-JSON) body
    timeout: float | None = None  # process normally, then hold the response this long
    delay: float | None = None  # hold before processing (idempotency key stays in flight)
    close: bool = False  # drop the connection without a response
    remaining: int | None = 1  # None = unlimited
    hits: int = 0

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "Fault":
        if not isinstance(obj, dict) or not isinstance(obj.get("match"), str):
            raise ValueError("each fault needs a string 'match'")
        count = obj.get("count", 1)
        if count is not None and (not isinstance(count, int) or count < 0):
            raise ValueError("count must be a non-negative integer or null")
        f = cls(match=obj["match"], target=obj.get("target", "resend"),
                status=obj.get("status"), name=obj.get("name"), message=obj.get("message"),
                retry_after=obj.get("retry_after"), body=obj.get("body"),
                timeout=obj.get("timeout"), delay=obj.get("delay"),
                close=bool(obj.get("close", False)), remaining=count)
        if f.target not in ("resend", "s3"):
            raise ValueError("target must be resend or s3")
        if f.status is None and f.timeout is None and f.delay is None and not f.close:
            raise ValueError("a fault needs status, timeout, delay or close")
        return f

    def to_json(self) -> dict[str, Any]:
        return {"match": self.match, "target": self.target, "status": self.status,
                "name": self.name, "message": self.message, "retry_after": self.retry_after,
                "body": self.body, "timeout": self.timeout, "delay": self.delay,
                "close": self.close, "count": self.remaining, "hits": self.hits}


@dataclass
class S3Object:
    data: bytes
    content_type: str
    etag: str
    last_modified: float
    metadata: dict[str, str]

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


class State:
    def __init__(self, config: dict[str, Any] | None = None) -> None:
        self.lock = threading.RLock()
        self.config: dict[str, Any] = dict(DEFAULT_CONFIG)
        if config:
            self.config.update(config)
        self.initial_config = dict(self.config)
        self.offset = 0.0
        self._seq = itertools.count(1)
        self.last_seq = 0
        self.emails: dict[str, SentEmail] = {}
        self.received: list[ReceivedEmail] = []  # insertion order = oldest first
        self.received_by_id: dict[str, ReceivedEmail] = {}
        self.idem: dict[str, IdemEntry] = {}
        self.faults: list[Fault] = []
        self.requests: deque[dict[str, Any]] = deque(maxlen=20000)
        self.downloads: dict[str, dict[str, Any]] = {}  # token -> {kind, received_id, att_id, exp}
        self.violations: list[dict[str, Any]] = []
        self.rate_window = (0, 0)  # (second, count)
        # S3
        self.buckets: set[str] = set()
        self.objects: dict[tuple[str, str], S3Object] = {}
        self.last_write: dict[tuple[str, str], float] = {}
        self.s3_requests: deque[dict[str, Any]] = deque(maxlen=20000)

    # -- clocks / sequence ----------------------------------------------------------------------
    @staticmethod
    def real() -> float:
        return time.time()

    def now(self) -> float:
        return time.time() + self.offset

    def next_seq(self) -> int:
        with self.lock:
            self.last_seq = next(self._seq)
            return self.last_seq

    # -- config ---------------------------------------------------------------------------------
    def update_config(self, patch: dict[str, Any]) -> dict[str, Any]:
        unknown = sorted(set(patch) - set(DEFAULT_CONFIG))
        if unknown:
            raise ValueError("unknown config keys: " + ", ".join(unknown))
        with self.lock:
            for key, value in patch.items():
                default = DEFAULT_CONFIG[key]
                if isinstance(default, bool):
                    if not isinstance(value, bool):
                        raise ValueError(f"{key} must be a boolean")
                elif isinstance(default, (int, float)) and not isinstance(default, bool):
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
                        raise ValueError(f"{key} must be a non-negative number")
                elif isinstance(default, list):
                    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
                        raise ValueError(f"{key} must be a list of strings")
                    value = [v.strip().lower() for v in value if v.strip()]
                elif isinstance(default, str) and not isinstance(value, str):
                    raise ValueError(f"{key} must be a string")
                self.config[key] = value
            return dict(self.config)

    def local_domains(self) -> set[str]:
        with self.lock:
            return {d.lower() for d in self.config["local_domains"]}

    def is_local(self, address: str) -> bool:
        return address.rsplit("@", 1)[-1].lower() in self.local_domains() if "@" in address else False

    # -- faults ---------------------------------------------------------------------------------
    def take_fault(self, target: str, method: str, path: str) -> Fault | None:
        key = f"{method.upper()} {path}"
        with self.lock:
            for fault in self.faults:
                if fault.target != target or fault.remaining == 0:
                    continue
                if fnmatch.fnmatchcase(key, fault.match):
                    if fault.remaining is not None:
                        fault.remaining -= 1
                    fault.hits += 1
                    return fault
        return None

    # -- rate limit -----------------------------------------------------------------------------
    def rate_limited(self) -> tuple[bool, int, int]:
        """Counts one request in the current 1 s window → (limited, limit, remaining)."""
        with self.lock:
            limit = int(self.config["rate_limit"])
            if limit <= 0:
                return False, 0, 0
            second = int(self.real())
            window, count = self.rate_window
            if window != second:
                window, count = second, 0
            count += 1
            self.rate_window = (window, count)
            return count > limit, limit, max(0, limit - count)

    # -- reset ----------------------------------------------------------------------------------
    def reset(self, keep_data: bool, wipe_s3: bool) -> None:
        with self.lock:
            self.config = dict(self.initial_config)
            self.faults.clear()
            self.offset = 0.0
            self.rate_window = (0, 0)
            self.violations.clear()
            if not keep_data:
                self.emails.clear()
                self.received.clear()
                self.received_by_id.clear()
                self.idem.clear()
                self.requests.clear()
                self.downloads.clear()
                self.s3_requests.clear()
            if wipe_s3:
                self.objects.clear()
                self.last_write.clear()
