"""Mock Resend API (+ minimal R2/S3 endpoint) for AZ Mail E2E tests and local development.

Run:  python3 -m tools.mock_resend.server --port 8787 --s3-port 8788 \\
          --webhook-url http://127.0.0.1:8080/api/webhooks/resend --local-domains azmail.test
(or ``python3 tools/mock_resend/server.py ...``). See README.md for the full behaviour list.
Python standard library only.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import hashlib
import heapq
import itertools
import json
import os
import random
import re
import secrets
import signal
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable

if __package__ in (None, ""):  # executed as a script: make the package importable
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    __package__ = "tools.mock_resend"  # noqa: A001

from . import eml, svix  # noqa: E402
from .s3 import S3Disk, S3Handler  # noqa: E402
from .store import Attachment, Fault, IdemEntry, ReceivedEmail, SentEmail, State  # noqa: E402

DEFAULT_API_KEY = "re_mock_0000000000000000000000000000"
DEFAULT_WEBHOOK_SECRET = "whsec_" + base64.b64encode(b"azmail-mock-webhook-secret-01").decode()
DEFAULT_S3_ACCESS_KEY = "mock-r2-access-key"
DEFAULT_S3_SECRET_KEY = "mock-r2-secret-key-0000000000000000000000"
MAX_RECIPIENTS = 50
MAX_TOTAL_ATTACHMENT_B64 = 40 * 1024 * 1024
MAX_SCHEDULE_DAYS = 30
IDEMPOTENCY_TTL = 24 * 3600
MAX_API_BODY = 64 * 1024 * 1024

EMAIL_RE = re.compile(r"^[^@\s<>\"(),;:]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+$")
TAG_RE = re.compile(r"^[A-Za-z0-9_-]{1,256}$")
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}(:\d{2}(\.\d{1,9})?)?(Z|[+-]\d{2}(:?\d{2})?)$")
PREFIX_OUTCOMES = ("bounce", "fail", "delay", "complain", "suppress")


class ApiErr(Exception):
    def __init__(self, status: int, name: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.name = name
        self.message = message


# ---------------------------------------------------------------------------------------------
# time formatting
# ---------------------------------------------------------------------------------------------

def iso_z(t: float) -> str:
    """2026-10-09T14:37:40.951Z"""
    d = dt.datetime.fromtimestamp(t, tz=dt.timezone.utc)
    return d.strftime("%Y-%m-%dT%H:%M:%S.") + f"{d.microsecond // 1000:03d}Z"


def pg_ts(t: float | None) -> str | None:
    """Postgres style, as GET /emails/{id} returns it: 2026-04-03 22:13:42.674981+00"""
    if t is None:
        return None
    d = dt.datetime.fromtimestamp(t, tz=dt.timezone.utc)
    return d.strftime("%Y-%m-%d %H:%M:%S.%f") + "+00"


def parse_iso(value: str) -> float:
    if not isinstance(value, str) or not ISO_RE.match(value.strip()):
        raise ValueError("not ISO 8601")
    s = value.strip().replace(" ", "T", 1)
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    m = re.search(r"([+-]\d{2})(\d{2})?$", s)
    if m and ":" not in s[m.start():]:
        s = s[:m.start()] + m.group(1) + ":" + (m.group(2) or "00")
    frac = re.search(r"\.(\d+)", s)
    if frac and len(frac.group(1)) > 6:
        s = s[:frac.start(1)] + frac.group(1)[:6] + s[frac.end(1):]
    return dt.datetime.fromisoformat(s).timestamp()


def parse_time_value(value: Any) -> float:
    """ms epoch number or ISO string -> epoch seconds."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value) / 1000.0
    return parse_iso(str(value))


# ---------------------------------------------------------------------------------------------
# delayed-callback scheduler
# ---------------------------------------------------------------------------------------------

class Scheduler:
    def __init__(self, time_scale: float) -> None:
        self._scale = max(0.0, time_scale)
        self._heap: list[tuple[float, int, Callable[[], None]]] = []
        self._cv = threading.Condition()
        self._seq = itertools.count()
        self._stop = False
        self._busy = 0
        self._thread = threading.Thread(target=self._run, name="mock-scheduler", daemon=True)
        self._thread.start()

    def call_later(self, delay: float, fn: Callable[[], None]) -> None:
        with self._cv:
            heapq.heappush(self._heap, (time.time() + max(0.0, delay) * self._scale,
                                        next(self._seq), fn))
            self._cv.notify_all()

    def idle(self) -> bool:
        with self._cv:
            return not self._heap and self._busy == 0

    def clear(self) -> None:
        with self._cv:
            self._heap.clear()

    def stop(self) -> None:
        with self._cv:
            self._stop = True
            self._cv.notify_all()
        self._thread.join(timeout=5)

    def _run(self) -> None:
        while True:
            with self._cv:
                while not self._stop and (not self._heap or self._heap[0][0] > time.time()):
                    wait = 0.5 if not self._heap else max(0.0, self._heap[0][0] - time.time())
                    self._cv.wait(min(wait, 0.5))
                if self._stop:
                    return
                _, _, fn = heapq.heappop(self._heap)
                self._busy += 1
            try:
                fn()
            except Exception as exc:  # noqa: BLE001 — keep the scheduler alive
                print(f"mock_resend: scheduled task failed: {exc!r}", file=sys.stderr)
            finally:
                with self._cv:
                    self._busy -= 1


# ---------------------------------------------------------------------------------------------
# the engine
# ---------------------------------------------------------------------------------------------

class MockResend:
    def __init__(self, *, api_key: str = DEFAULT_API_KEY, webhook_url: str = "",
                 webhook_secret: str = DEFAULT_WEBHOOK_SECRET, local_domains: list[str] | None = None,
                 time_scale: float = 1.0, meta_delay: float = 0.0, rate_limit: int = 10,
                 reject_scheduled_attachments: bool = False,
                 s3_access_key: str = DEFAULT_S3_ACCESS_KEY, s3_secret_key: str = DEFAULT_S3_SECRET_KEY,
                 s3_buckets: list[str] | None = None, s3_regions: list[str] | None = None,
                 s3_write_interval: float = 1.0, s3_max_object_mb: int = 200,
                 s3_data_dir: str | None = None, verbose: bool = False) -> None:
        svix.sign(webhook_secret, "msg_check", "0", b"")  # validates the secret format early
        self.api_key = api_key
        self.webhook_secret = webhook_secret
        self.time_scale = time_scale
        self.verbose = verbose
        self.state = State({
            "webhook_url": webhook_url,
            "local_domains": [d.strip().lower() for d in (local_domains or []) if d.strip()],
            "meta_delay": float(meta_delay),
            "rate_limit": int(rate_limit),
            "reject_scheduled_attachments": bool(reject_scheduled_attachments),
        })
        self.s3_keys = {s3_access_key: s3_secret_key}
        self.s3_regions = list(s3_regions or ["auto"])
        self.s3_write_interval = s3_write_interval
        self.s3_max_object_bytes = s3_max_object_mb * 1024 * 1024
        self.state.buckets = set(s3_buckets or ["azmail"])
        self.s3_disk = S3Disk(s3_data_dir) if s3_data_dir else None
        if self.s3_disk is not None:
            for bucket, key, obj in self.s3_disk.load():
                self.state.objects[(bucket, key)] = obj
        self.sender = svix.WebhookSender(self._webhook_target, time_scale)
        self.scheduler = Scheduler(time_scale)
        self._servers: list[ThreadingHTTPServer] = []
        self._fire_lock = threading.Lock()  # /_mock/advance and the ticker never interleave
        self._ticker_stop = threading.Event()
        self._ticker = threading.Thread(target=self._tick_loop, name="mock-ticker", daemon=True)
        self._ticker.start()

    # -- lifecycle ------------------------------------------------------------------------------
    def serve(self, host: str = "127.0.0.1", port: int = 0, s3_port: int | None = None) -> tuple[int, int | None]:
        api_handler = type("BoundApiHandler", (ApiHandler,), {"mock": self})
        api = ThreadingHTTPServer((host, port), api_handler)
        api.daemon_threads = True
        self._servers.append(api)
        threading.Thread(target=api.serve_forever, name="mock-api", daemon=True).start()
        bound_s3 = None
        if s3_port is not None and s3_port >= 0:
            s3_handler = type("BoundS3Handler", (S3Handler,), {"mock": self})
            s3 = ThreadingHTTPServer((host, s3_port), s3_handler)
            s3.daemon_threads = True
            self._servers.append(s3)
            threading.Thread(target=s3.serve_forever, name="mock-s3", daemon=True).start()
            bound_s3 = s3.server_address[1]
        return api.server_address[1], bound_s3

    def shutdown(self) -> None:
        self._ticker_stop.set()
        for srv in self._servers:
            srv.shutdown()
            srv.server_close()
        self.scheduler.stop()
        self.sender.stop()

    def _webhook_target(self) -> tuple[str, str]:
        with self.state.lock:
            return self.state.config["webhook_url"], self.webhook_secret

    def _tick_loop(self) -> None:
        while not self._ticker_stop.wait(0.2):
            try:
                with self._fire_lock:
                    self.fire_due()
            except Exception as exc:  # noqa: BLE001
                print(f"mock_resend: ticker failed: {exc!r}", file=sys.stderr)

    def idle(self) -> bool:
        return self.scheduler.idle() and self.sender.pending() == 0

    # -- helpers --------------------------------------------------------------------------------
    @property
    def cfg(self) -> dict[str, Any]:
        return self.state.config

    def _classify(self, address: str) -> str:
        local = eml.local_part(address)
        for prefix in PREFIX_OUTCOMES:
            if local.startswith(prefix):
                return prefix
        return "ok"

    # -- sending --------------------------------------------------------------------------------
    def create_email(self, req: dict[str, Any], idempotency_key: str | None, user_agent: str) -> dict[str, Any]:
        st = self.state
        from_ = req.get("from")
        if from_ is None:
            raise ApiErr(422, "missing_required_field", "Missing `from` field.")
        if not isinstance(from_, str) or not EMAIL_RE.match(eml.addr_of(from_)):
            raise ApiErr(422, "validation_error",
                         "Invalid `from` field. The email address needs to follow the "
                         "`email@example.com` or `Name <email@example.com>` format.")
        if self.cfg["verify_from_domain"]:
            domain = eml.domain_of(eml.addr_of(from_))
            if domain not in st.local_domains():
                raise ApiErr(403, "validation_error",
                             f"The {domain} domain is not verified. Please, add and verify your "
                             "domain on https://resend.com/domains")
        to = self._addresses(req, "to", required=True)
        cc = self._addresses(req, "cc")
        bcc = self._addresses(req, "bcc")
        reply_to = self._addresses(req, "reply_to")
        if "subject" not in req or req["subject"] is None:
            raise ApiErr(422, "missing_required_field", "Missing `subject` field.")
        if not isinstance(req["subject"], str):
            raise ApiErr(422, "validation_error", "Invalid `subject` field.")
        html, text = req.get("html"), req.get("text")
        for name, value in (("html", html), ("text", text)):
            if value is not None and not isinstance(value, str):
                raise ApiErr(422, "validation_error", f"Invalid `{name}` field.")
        if html is None and text is None:
            raise ApiErr(422, "missing_required_field", "Missing `html` or `text` field.")
        headers = req.get("headers") or {}
        if not isinstance(headers, dict) or not all(
                isinstance(k, str) and isinstance(v, str) for k, v in headers.items()):
            raise ApiErr(422, "validation_error", "`headers` must be an object of string values.")
        tags = self._tags(req.get("tags"))
        attachments = self._attachments(req.get("attachments"))
        scheduled_at = None
        if req.get("scheduled_at") is not None:
            scheduled_at = self._schedule_time(req["scheduled_at"])
            if attachments and self.cfg["reject_scheduled_attachments"]:
                raise ApiErr(422, "validation_error", "Emails with attachments cannot be scheduled.")

        message_id = None
        if self.cfg["honor_message_id"]:
            for k, v in headers.items():
                if k.lower() == "message-id":
                    message_id = eml.angle(v)
        if not message_id:
            message_id = f"<{secrets.token_hex(16)}@email.mock-resend.local>"
        email = SentEmail(
            id=str(uuid.uuid4()), created_at=st.now(), from_=from_, to=to, cc=cc, bcc=bcc,
            reply_to=reply_to, subject=req["subject"], html=html, text=text,
            headers=list(headers.items()), attachments=attachments, tags=tags,
            scheduled_at=scheduled_at, message_id=message_id, message_id_at=None,
            last_event="queued", idempotency_key=idempotency_key, user_agent=user_agent,
            seq=st.next_seq())
        with st.lock:
            st.emails[email.id] = email
        if scheduled_at is not None:
            email.last_event = "scheduled"
            self._emit(email, "email.scheduled")
        else:
            self._dispatch(email)
        return {"id": email.id}

    def _addresses(self, req: dict[str, Any], field: str, required: bool = False) -> list[str]:
        value = req.get(field)
        if value is None or value == []:
            if required:
                raise ApiErr(422, "missing_required_field", f"Missing `{field}` field.")
            return []
        items = [value] if isinstance(value, str) else value
        if not isinstance(items, list) or not all(isinstance(v, str) for v in items):
            raise ApiErr(422, "validation_error", f"Invalid `{field}` field.")
        if len(items) > MAX_RECIPIENTS:
            raise ApiErr(422, "validation_error",
                         f"Too many recipients in `{field}`. The maximum is {MAX_RECIPIENTS}.")
        for item in items:
            if not EMAIL_RE.match(eml.addr_of(item)):
                raise ApiErr(422, "validation_error",
                             f"Invalid `{field}` field. The email address needs to follow the "
                             "`email@example.com` or `Name <email@example.com>` format.")
        return list(items)

    @staticmethod
    def _tags(value: Any) -> dict[str, str]:
        if value is None:
            return {}
        if not isinstance(value, list):
            raise ApiErr(422, "validation_error", "`tags` must be an array of {name, value}.")
        out: dict[str, str] = {}
        for tag in value:
            if not isinstance(tag, dict) or not isinstance(tag.get("name"), str) or \
                    not isinstance(tag.get("value"), str):
                raise ApiErr(422, "validation_error", "`tags` must be an array of {name, value}.")
            if not TAG_RE.match(tag["name"]) or not TAG_RE.match(tag["value"]):
                raise ApiErr(422, "validation_error",
                             "Tags should only contain ASCII letters, numbers, underscores, or "
                             "dashes, and be at most 256 characters.")
            out[tag["name"]] = tag["value"]
        return out

    @staticmethod
    def _attachments(value: Any) -> list[Attachment]:
        if value is None:
            return []
        if not isinstance(value, list):
            raise ApiErr(422, "invalid_attachment", "`attachments` must be an array.")
        out: list[Attachment] = []
        total = 0
        for item in value:
            if not isinstance(item, dict):
                raise ApiErr(422, "invalid_attachment", "Invalid attachment.")
            if item.get("path") is not None and item.get("content") is None:
                raise ApiErr(422, "invalid_attachment",
                             "mock: attachments by `path` are not supported (AZ Mail always sends `content`).")
            filename = item.get("filename")
            content = item.get("content")
            if not isinstance(filename, str) or not filename:
                raise ApiErr(422, "invalid_attachment", "Attachment must have a `filename`.")
            if not isinstance(content, str):
                raise ApiErr(422, "invalid_attachment", "Attachment must have either a `content` or `path`.")
            try:
                data = base64.b64decode(content, validate=True)
            except (binascii.Error, ValueError):
                raise ApiErr(422, "invalid_attachment",
                             f"Attachment `{filename}` content is not valid base64.") from None
            total += len(content)
            cid = item.get("content_id")
            if cid is not None:
                if not isinstance(cid, str) or not cid:
                    raise ApiErr(422, "validation_error", "Invalid `content_id`.")
                if len(cid) >= 128:
                    raise ApiErr(422, "validation_error", "`content_id` must be less than 128 characters.")
            ctype = item.get("content_type") or eml.guess_type(filename)
            if not isinstance(ctype, str):
                raise ApiErr(422, "invalid_attachment", "Invalid `content_type`.")
            out.append(Attachment(id=str(uuid.uuid4()), filename=filename, content_type=ctype,
                                  data=data, content_id=cid,
                                  disposition="inline" if cid else "attachment"))
        if total > MAX_TOTAL_ATTACHMENT_B64:
            raise ApiErr(422, "validation_error", "Total email size exceeds 40MB (after base64).")
        return out

    def _schedule_time(self, value: Any) -> float:
        if not isinstance(value, str):
            raise ApiErr(422, "validation_error", "Invalid `scheduled_at` field.")
        try:
            at = parse_iso(value)
        except ValueError:
            raise ApiErr(422, "validation_error",
                         "mock: `scheduled_at` must be ISO 8601 with a timezone (natural language is "
                         "valid on Resend, but AZ Mail must never send it).") from None
        now = self.state.now()
        if at < now - 5 or at > now + MAX_SCHEDULE_DAYS * 86400:
            raise ApiErr(422, "validation_error",
                         "`scheduled_at` must be in the future and within 30 days.")
        return at

    # -- delivery simulation ---------------------------------------------------------------------
    def _dispatch(self, email: SentEmail) -> None:
        """Starts the delivery simulation (immediately for normal sends; at schedule time)."""
        with self.state.lock:
            if email.dispatched:
                return
            email.dispatched = True
            email.message_id_at = time.time() + float(self.cfg["meta_delay"])
            spacing = float(self.cfg["event_spacing"])
            first = float(self.cfg["delivery_delay"])
            shuffle = bool(self.cfg["shuffle_events"])
        rcpts = [eml.addr_of(r) for r in email.recipients()]
        local = [r for r in rcpts if self.state.is_local(r)]
        kinds = {self._classify(r) for r in rcpts if not self.state.is_local(r)}
        events: list[str] = []
        late: list[str] = []
        if "fail" in kinds:
            events = ["email.failed"]
        else:
            events.append("email.sent")
            if "suppress" in kinds:
                events.append("email.suppressed")
            if "bounce" in kinds:
                events.append("email.bounced")
            needs_delivered = bool(local) or bool(kinds & {"ok", "complain", "delay"})
            if "delay" in kinds:
                events.append("email.delivery_delayed")
                late.append("email.delivered")
                if "complain" in kinds:
                    late.append("email.complained")
            else:
                if needs_delivered:
                    events.append("email.delivered")
                if "complain" in kinds:
                    events.append("email.complained")
        if shuffle:
            random.shuffle(events)
        for i, etype in enumerate(events):
            self.scheduler.call_later(first + i * spacing, lambda t=etype: self._emit(email, t))
        for i, etype in enumerate(late):
            self.scheduler.call_later(first + len(events) * spacing + 2.0 + i * spacing,
                                      lambda t=etype: self._emit(email, t))
        if local and "fail" not in kinds:
            self.scheduler.call_later(first, lambda: self._loopback(email))

    def _event_data(self, email: SentEmail) -> dict[str, Any]:
        data: dict[str, Any] = {
            "created_at": iso_z(email.created_at),
            "email_id": email.id,
            "from": email.from_,
            "to": list(email.to),
            "subject": email.subject,
            "tags": dict(email.tags),
        }
        mid = email.message_id_visible(time.time())
        if mid is not None:
            data["message_id"] = mid
        return data

    def _emit(self, email: SentEmail, etype: str, svix_id: str | None = None) -> str | None:
        data = self._event_data(email)
        if etype == "email.bounced":
            data["bounce"] = {"message": "550 5.1.1 The email account that you tried to reach "
                                         "does not exist. (mock)",
                              "subType": "General", "type": "Permanent"}
        elif etype == "email.failed":
            data["failed"] = {"reason": "mock_failure"}
        elif etype == "email.suppressed":
            data["suppressed"] = {"message": "Resend has suppressed sending to this address "
                                             "because it is on the account-level suppression list. (mock)",
                                  "type": "OnAccountSuppressionList"}
        if etype != "email.scheduled":
            with self.state.lock:
                email.last_event = etype.split(".", 1)[1]
        payload = {"type": etype, "created_at": iso_z(time.time()), "data": data}
        return self._send_webhook(payload, svix_id, email_events=email.events)

    def _send_webhook(self, payload: dict[str, Any], svix_id: str | None = None,
                      email_events: list[dict[str, Any]] | None = None) -> str | None:
        with self.state.lock:
            enabled = bool(self.cfg["webhooks_enabled"])
            copies = 2 if self.cfg["duplicate_webhooks"] else 1
        sid = svix_id or svix.new_msg_id()
        record = {"type": payload["type"], "at": time.time(), "svix_id": sid,
                  "webhook": "sent" if enabled else "suppressed"}
        if email_events is not None:
            with self.state.lock:
                email_events.append(record)
        if enabled:
            self.sender.enqueue(payload, svix_id=sid, copies=copies)
            return sid
        return None

    def _loopback(self, email: SentEmail) -> None:
        cfg = self.cfg
        headers: list[tuple[str, str]] = []
        for name, value in email.headers:
            lname = name.lower()
            if lname == "message-id":
                continue
            if cfg["strip_custom_headers"] and lname.startswith("x-"):
                continue
            if cfg["strip_thread_headers"] and lname in ("in-reply-to", "references"):
                continue
            headers.append((name, value))
        rcpts: list[str] = []
        for r in email.recipients():
            a = eml.addr_of(r)
            if self.state.is_local(a) and a not in rcpts:
                rcpts.append(a)
        from_domain = eml.domain_of(eml.addr_of(email.from_))
        parts = [eml.Part(a.filename, a.content_type, a.data, a.content_id, bool(a.content_id))
                 for a in email.attachments]
        raw = eml.build_eml(from_=email.from_, to=email.to, cc=email.cc, reply_to=email.reply_to,
                            subject=email.subject, html=email.html, text=email.text,
                            message_id=email.message_id, date=self.state.now(), headers=headers,
                            attachments=parts,
                            auth_results=eml.auth_results_header("pass", "pass", "pass", from_domain))
        self._store_received(
            from_=email.from_, to=email.to, cc=email.cc, bcc=email.bcc, reply_to=email.reply_to,
            envelope=rcpts, subject=email.subject, message_id=email.message_id, html=email.html,
            text=email.text, raw=raw, attachments=email.attachments,
            authentication={"spf": "pass", "dkim": "pass", "dmarc": "pass"},
            source="loopback", sent_email_id=email.id, created_at=self.state.now(), notify=True)

    def _store_received(self, *, from_: str, to: list[str], cc: list[str], bcc: list[str],
                        reply_to: list[str], envelope: list[str], subject: str, message_id: str,
                        html: str | None, text: str | None, raw: bytes,
                        attachments: list[Attachment], authentication: dict[str, str] | None,
                        source: str, sent_email_id: str | None, created_at: float,
                        notify: bool) -> list[ReceivedEmail]:
        groups = [[r] for r in envelope] if self.cfg["split_delivery"] and envelope else [envelope]
        stored: list[ReceivedEmail] = []
        for group in groups:
            rec = ReceivedEmail(
                id=str(uuid.uuid4()), created_at=created_at, from_=from_, to=list(to), cc=list(cc),
                bcc=list(bcc), reply_to=list(reply_to), received_for=list(group), subject=subject,
                message_id=eml.angle(message_id), html=html, text=text, headers=eml.headers_map(raw),
                authentication=authentication, raw=raw,
                attachments=[Attachment(id=str(uuid.uuid4()), filename=a.filename,
                                        content_type=a.content_type, data=a.data,
                                        content_id=a.content_id, disposition=a.disposition)
                             for a in attachments],
                source=source, sent_email_id=sent_email_id, seq=self.state.next_seq())
            with self.state.lock:
                self.state.received.append(rec)
                self.state.received_by_id[rec.id] = rec
            stored.append(rec)
            if notify:
                self.notify_received(rec)
        return stored

    def notify_received(self, rec: ReceivedEmail, svix_id: str | None = None) -> str | None:
        payload = {
            "type": "email.received",
            "created_at": iso_z(time.time()),
            "data": {
                "email_id": rec.id,
                "created_at": iso_z(rec.created_at),
                "from": rec.from_,
                "to": rec.to,
                "bcc": rec.bcc,
                "cc": rec.cc,
                "received_for": rec.received_for,
                "message_id": rec.message_id,
                "subject": rec.subject,
                "attachments": [{"id": a.id, "filename": a.filename, "content_type": a.content_type,
                                 "content_disposition": a.disposition, "content_id": a.content_id}
                                for a in rec.attachments],
            },
        }
        return self._send_webhook(payload, svix_id)

    def fire_due(self) -> list[str]:
        """Dispatches scheduled emails whose scheduled_at has passed on the mock clock."""
        now = self.state.now()
        with self.state.lock:
            due = [e for e in self.state.emails.values()
                   if e.last_event == "scheduled" and not e.dispatched and e.scheduled_at is not None
                   and e.scheduled_at <= now]
        for email in due:
            self._dispatch(email)
        return [e.id for e in due]

    # -- inbound injection ----------------------------------------------------------------------
    def inject(self, body: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(body, dict):
            raise ApiErr(400, "validation_error", "body must be an object")
        from_ = body.get("from")
        if not isinstance(from_, str) or not EMAIL_RE.match(eml.addr_of(from_)):
            raise ApiErr(400, "validation_error", "`from` must be an email address")

        def addr_list(key: str) -> list[str]:
            v = body.get(key) or []
            v = [v] if isinstance(v, str) else v
            if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
                raise ApiErr(400, "validation_error", f"`{key}` must be a string or list of strings")
            return v

        to, cc, bcc, reply_to = addr_list("to"), addr_list("cc"), addr_list("bcc"), addr_list("reply_to")
        received_for = [eml.addr_of(a) for a in addr_list("received_for")]
        if not received_for:
            received_for = []
            for a in to + cc + bcc:
                b = eml.addr_of(a)
                if self.state.is_local(b) and b not in received_for:
                    received_for.append(b)
        if not to and not received_for:
            raise ApiErr(400, "validation_error", "`to` or `received_for` is required")
        subject = body.get("subject", "")
        html, text = body.get("html"), body.get("text")
        if html is None and text is None:
            text = ""
        created = parse_time_value(body["date"]) if body.get("date") is not None else self.state.now()
        parts: list[Attachment] = []
        for item in body.get("attachments") or []:
            if "content" in item:
                data = base64.b64decode(item["content"], validate=True)
            else:
                data = str(item.get("text", "")).encode("utf-8")
            filename = item.get("filename") or "attachment.bin"
            cid = item.get("content_id")
            inline = bool(item.get("inline", cid is not None))
            parts.append(Attachment(id="", filename=filename,
                                    content_type=item.get("content_type") or eml.guess_type(filename),
                                    data=data, content_id=cid,
                                    disposition="inline" if inline and cid else "attachment"))
        headers: list[tuple[str, str]] = []
        if body.get("in_reply_to"):
            headers.append(("In-Reply-To", body["in_reply_to"]))
        if body.get("references"):
            refs = body["references"]
            headers.append(("References", " ".join(refs) if isinstance(refs, list) else refs))
        for k, v in (body.get("headers") or {}).items():
            headers.append((k, v))
        message_id = eml.angle(body.get("message_id") or eml.new_message_id(eml.domain_of(eml.addr_of(from_)) or "mock.test"))
        spf = body.get("spf", "pass")
        dkim = body.get("dkim", "pass")
        dmarc = body.get("dmarc", "pass")
        from_domain = eml.domain_of(eml.addr_of(from_))
        raw = eml.build_eml(from_=from_, to=to, cc=cc, reply_to=reply_to, subject=subject, html=html,
                            text=text, message_id=message_id, date=created, headers=headers,
                            attachments=[eml.Part(p.filename, p.content_type, p.data, p.content_id,
                                                  p.disposition == "inline") for p in parts],
                            auth_results=eml.auth_results_header(spf, dkim, dmarc, from_domain),
                            subject_charset=body.get("subject_charset"))
        recs = self._store_received(
            from_=from_, to=to, cc=cc, bcc=bcc, reply_to=reply_to, envelope=received_for,
            subject=subject, message_id=message_id, html=html, text=text, raw=raw, attachments=parts,
            authentication={"spf": spf, "dkim": dkim, "dmarc": dmarc}, source="inject",
            sent_email_id=None, created_at=created, notify=bool(body.get("webhook", True)))
        return {"ids": [r.id for r in recs], "message_id": message_id,
                "raw_sha256": hashlib.sha256(raw).hexdigest()}

    # -- download tokens ------------------------------------------------------------------------
    def new_download(self, kind: str, rec: ReceivedEmail, att: Attachment | None, host: str) -> tuple[str, float]:
        token = secrets.token_urlsafe(18)
        exp = int(time.time() + float(self.cfg["download_ttl"]))
        with self.state.lock:
            self.state.downloads[token] = {"kind": kind, "received_id": rec.id,
                                           "att_id": att.id if att else None, "exp": exp}
        return f"http://{host}/_dl/{token}?exp={exp}", float(exp)


# ---------------------------------------------------------------------------------------------
# HTTP handler (Resend API + control endpoints + download URLs)
# ---------------------------------------------------------------------------------------------

def _json_bytes(obj: Any) -> bytes:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _sanitize_request_body(obj: Any) -> Any:
    """Request body for the /_mock/sent log: attachment bytes replaced by length + sha256."""
    if not isinstance(obj, dict):
        return obj
    out = dict(obj)
    atts = out.get("attachments")
    if isinstance(atts, list):
        clean = []
        for a in atts:
            if isinstance(a, dict) and isinstance(a.get("content"), str):
                b = dict(a)
                content = b.pop("content")
                b["content_b64_length"] = len(content)
                try:
                    b["sha256"] = hashlib.sha256(base64.b64decode(content, validate=True)).hexdigest()
                except (binascii.Error, ValueError):
                    b["sha256"] = None
                clean.append(b)
            else:
                clean.append(a)
        out["attachments"] = clean
    return out


class ApiHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockResend/1.0"
    mock: MockResend  # bound by MockResend.serve

    def log_message(self, fmt: str, *args: Any) -> None:
        if self.mock.verbose:
            super().log_message(fmt, *args)

    def do_GET(self) -> None:
        self._dispatch("GET")

    def do_POST(self) -> None:
        self._dispatch("POST")

    def do_PATCH(self) -> None:
        self._dispatch("PATCH")

    def do_PUT(self) -> None:
        self._dispatch("PUT")

    def do_DELETE(self) -> None:
        self._dispatch("DELETE")

    def do_HEAD(self) -> None:
        self._dispatch("HEAD")

    # -- I/O ------------------------------------------------------------------------------------
    def _read_body(self) -> bytes:
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            chunks = []
            total = 0
            while True:
                line = self.rfile.readline(65537).strip()
                size = int(line.split(b";")[0] or b"0", 16)
                if size == 0:
                    while self.rfile.readline(65537) not in (b"\r\n", b"\n", b""):
                        pass
                    break
                total += size
                if total > MAX_API_BODY:
                    raise ApiErr(413, "validation_error", "Request body too large.")
                chunks.append(self.rfile.read(size))
                self.rfile.readline(65537)
            return b"".join(chunks)
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_API_BODY:
            self.close_connection = True
            raise ApiErr(413, "validation_error", "Request body too large.")
        return self.rfile.read(length) if length else b""

    def _send(self, status: int, body: bytes, content_type: str = "application/json; charset=utf-8",
              headers: dict[str, str] | None = None, method: str = "GET") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        merged = dict(getattr(self, "_extra_headers", None) or {})
        merged.update(headers or {})
        for k, v in merged.items():
            self.send_header(k, v)
        self.end_headers()
        if method != "HEAD" and body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True

    def _json(self, status: int, obj: Any, headers: dict[str, str] | None = None,
              method: str = "GET") -> bytes:
        body = _json_bytes(obj)
        self._send(status, body, headers=headers, method=method)
        return body

    def _api_error(self, status: int, name: str, message: str,
                   headers: dict[str, str] | None = None) -> bytes:
        return self._json(status, {"statusCode": status, "name": name, "message": message}, headers)

    # -- dispatch -------------------------------------------------------------------------------
    def _dispatch(self, method: str) -> None:
        self._extra_headers: dict[str, str] = {}
        split = urllib.parse.urlsplit(self.path)
        path = split.path
        query = dict(urllib.parse.parse_qsl(split.query, keep_blank_values=True))
        try:
            body = self._read_body()
        except ApiErr as err:
            self._api_error(err.status, err.name, err.message)
            return
        except (ValueError, OSError):
            self.close_connection = True
            self._api_error(400, "validation_error", "Malformed request body.")
            return
        if path.startswith("/_mock/") or path == "/_mock":
            self._control(method, path, query, body)
        elif path.startswith("/_dl/"):
            self._download(method, path, query, final=False)
        elif path.startswith("/_blob/"):
            self._download(method, path, query, final=True)
        else:
            self._api(method, path, query, body)

    # -- Resend API -----------------------------------------------------------------------------
    def _api(self, method: str, path: str, query: dict[str, str], body: bytes) -> None:
        mock = self.mock
        st = mock.state
        ua = self.headers.get("User-Agent", "")
        idem_key = self.headers.get("Idempotency-Key")
        parsed_body: Any = None
        if body:
            try:
                parsed_body = json.loads(body)
            except ValueError:
                parsed_body = None
        log: dict[str, Any] = {
            "seq": st.next_seq(), "at": time.time(), "method": method, "path": path,
            "query": query, "user_agent": ua, "idempotency_key": idem_key,
            "auth": None, "status": None, "name": None, "email_id": None, "replayed": False,
            "fault": None, "body": _sanitize_request_body(parsed_body) if parsed_body is not None else None,
        }
        with st.lock:
            st.requests.append(log)

        if not ua.strip():
            # Cloudflare in front of Resend rejects requests without a User-Agent (non-JSON body).
            log.update(status=403, name="cloudflare_1010")
            self._send(403, b"error code: 1010", "text/plain; charset=UTF-8", method=method)
            return
        auth = self.headers.get("Authorization", "")
        if not auth:
            log.update(auth="missing", status=401, name="missing_api_key")
            self._api_error(401, "missing_api_key", "Missing API key in the authorization header.")
            return
        if auth != f"Bearer {mock.api_key}":
            log.update(auth="invalid", status=403, name="invalid_api_key")
            self._api_error(403, "invalid_api_key", "API key is invalid.")
            return
        log["auth"] = "ok"
        limited, limit, remaining = st.rate_limited()
        if limit:
            self._extra_headers = {"ratelimit-limit": str(limit), "ratelimit-remaining": str(remaining),
                                   "ratelimit-reset": "1"}
        if limited:
            log.update(status=429, name="rate_limit_exceeded")
            self._api_error(429, "rate_limit_exceeded",
                            "Too many requests. You can only make 10 requests per second. See rate "
                            "limit response headers for more information. Or contact support to "
                            "increase rate limit.", {"retry-after": "1"})
            return

        fault = st.take_fault("resend", method, path)
        if fault is not None:
            log["fault"] = fault.match
            if fault.close:
                log["status"] = "closed"
                self.close_connection = True
                return
            if fault.status is not None:
                log.update(status=fault.status, name=fault.name)
                if fault.body is not None:
                    self._send(fault.status, fault.body.encode(), "text/html; charset=UTF-8")
                    return
                headers = {}
                if fault.retry_after is not None or fault.status == 429:
                    headers["retry-after"] = str(fault.retry_after if fault.retry_after is not None else 1)
                self._api_error(fault.status, fault.name or "application_error",
                                fault.message or "Injected fault (mock).", headers)
                return

        try:
            status, payload, replayed = self._route(method, path, query, body, idem_key, ua, fault)
        except ApiErr as err:
            log.update(status=err.status, name=err.name)
            body_out = self._api_error_body(err)
            self._hold(fault)
            self._send(err.status, body_out)
            return
        log.update(status=status, replayed=replayed)
        out = payload if isinstance(payload, bytes) else _json_bytes(payload)
        if method == "POST" and path == "/emails" and 200 <= status < 300:
            try:
                log["email_id"] = json.loads(out).get("id")
            except (ValueError, AttributeError):
                pass
        self._hold(fault)
        self._send(status, out, method=method)

    @staticmethod
    def _api_error_body(err: ApiErr) -> bytes:
        return _json_bytes({"statusCode": err.status, "name": err.name, "message": err.message})

    def _hold(self, fault: Fault | None) -> None:
        if fault is not None and fault.timeout:
            time.sleep(float(fault.timeout))

    def _route(self, method: str, path: str, query: dict[str, str], body: bytes,
               idem_key: str | None, ua: str, fault: Fault | None) -> tuple[int, Any, bool]:
        parts = [p for p in path.split("/") if p]
        if fault is not None and fault.delay and not (method == "POST" and parts == ["emails"]):
            time.sleep(float(fault.delay))
        if parts == ["emails"]:
            if method == "POST":
                return self._post_emails(body, idem_key, ua, fault)
            if method == "GET":
                return 200, self._list_sent(query), False
            raise ApiErr(405, "method_not_allowed", "Method not allowed.")
        if len(parts) >= 2 and parts[0] == "emails" and parts[1] == "receiving":
            if method != "GET":
                raise ApiErr(405, "method_not_allowed", "Method not allowed.")
            return 200, self._receiving(parts[2:], query), False
        if len(parts) == 2 and parts[0] == "emails":
            if method == "GET":
                return 200, self._get_email(parts[1]), False
            if method == "PATCH":
                return 200, self._patch_email(parts[1], body), False
            raise ApiErr(405, "method_not_allowed", "Method not allowed.")
        if len(parts) == 3 and parts[0] == "emails" and parts[2] == "cancel":
            if method != "POST":
                raise ApiErr(405, "method_not_allowed", "Method not allowed.")
            return 200, self._cancel_email(parts[1]), False
        if parts and parts[0] == "domains" and method == "GET" and len(parts) <= 2:
            return 200, self._domains(parts[1] if len(parts) == 2 else None), False
        if parts == ["api-keys"] and method == "GET":
            return 200, {"object": "list", "has_more": False,
                         "data": [{"id": "b6d24b8e-af0b-4c3c-be0c-359bbd97381e",
                                   "name": "mock", "created_at": "2026-01-01 00:00:00.000000+00"}]}, False
        raise ApiErr(404, "not_found", "The requested endpoint does not exist.")

    def _post_emails(self, body: bytes, idem_key: str | None, ua: str,
                     fault: Fault | None) -> tuple[int, Any, bool]:
        st = self.mock.state
        try:
            req = json.loads(body or b"null")
        except ValueError:
            raise ApiErr(422, "validation_error", "The request body is not valid JSON.") from None
        if not isinstance(req, dict):
            raise ApiErr(422, "validation_error", "The request body must be a JSON object.")
        entry: IdemEntry | None = None
        if idem_key is not None:
            if not 1 <= len(idem_key) <= 256:
                raise ApiErr(400, "invalid_idempotency_key",
                             "The key must be between 1-256 chars.")
            body_hash = hashlib.sha256(json.dumps(req, sort_keys=True, ensure_ascii=False,
                                                  separators=(",", ":")).encode()).hexdigest()
            with st.lock:
                existing = st.idem.get(idem_key)
                if existing is not None and st.now() - existing.created > IDEMPOTENCY_TTL:
                    st.idem.pop(idem_key, None)
                    existing = None
                if existing is not None:
                    if existing.body_hash != body_hash:
                        raise ApiErr(409, "invalid_idempotent_request",
                                     "Same idempotency key used with a different request payload.")
                    if existing.state == "in_flight":
                        raise ApiErr(409, "concurrent_idempotent_requests",
                                     "Same idempotency key used while original request is still in progress.")
                    return existing.status, existing.body, True
                entry = IdemEntry(body_hash=body_hash, created=st.now(), state="in_flight")
                st.idem[idem_key] = entry
        try:
            if fault is not None and fault.delay:
                time.sleep(float(fault.delay))
            payload = self.mock.create_email(req, idem_key, ua)
        except ApiErr:
            if entry is not None:
                with st.lock:
                    if st.idem.get(idem_key) is entry:  # type: ignore[arg-type]
                        st.idem.pop(idem_key, None)  # type: ignore[arg-type]
            raise
        out = _json_bytes(payload)
        if entry is not None:
            with st.lock:
                entry.state, entry.status, entry.body = "done", 200, out
        return 200, out, False

    def _email(self, email_id: str) -> SentEmail:
        with self.mock.state.lock:
            email = self.mock.state.emails.get(email_id)
        if email is None:
            raise ApiErr(404, "not_found", "Email not found")
        return email

    def _get_email(self, email_id: str) -> dict[str, Any]:
        e = self._email(email_id)
        with self.mock.state.lock:
            return {
                "object": "email", "id": e.id, "message_id": e.message_id_visible(time.time()),
                "to": e.to, "from": e.from_, "created_at": pg_ts(e.created_at),
                "subject": e.subject, "html": e.html, "text": e.text, "bcc": e.bcc, "cc": e.cc,
                "reply_to": e.reply_to, "last_event": e.last_event,
                "scheduled_at": pg_ts(e.scheduled_at),
                "tags": [{"name": k, "value": v} for k, v in e.tags.items()],
            }

    def _list_sent(self, query: dict[str, str]) -> dict[str, Any]:
        limit = _limit(query.get("limit"), default=20)
        with self.mock.state.lock:
            items = sorted(self.mock.state.emails.values(), key=lambda e: e.seq, reverse=True)
        sel, more = _page(items, limit, query.get("after"), query.get("before"), lambda e: e.id)
        return {"object": "list", "has_more": more, "data": [
            {"id": e.id, "to": e.to, "from": e.from_, "created_at": pg_ts(e.created_at),
             "subject": e.subject, "bcc": e.bcc, "cc": e.cc, "reply_to": e.reply_to,
             "last_event": e.last_event, "scheduled_at": pg_ts(e.scheduled_at)} for e in sel]}

    def _patch_email(self, email_id: str, body: bytes) -> dict[str, Any]:
        e = self._email(email_id)
        try:
            req = json.loads(body or b"{}")
        except ValueError:
            raise ApiErr(422, "validation_error", "The request body is not valid JSON.") from None
        if not isinstance(req, dict) or "scheduled_at" not in req:
            raise ApiErr(422, "missing_required_field", "Missing `scheduled_at` field.")
        at = self.mock._schedule_time(req["scheduled_at"])
        with self.mock.state.lock:
            if e.last_event != "scheduled" or e.dispatched:
                raise ApiErr(422, "validation_error",
                             "Email cannot be updated because it is not scheduled (already sent or canceled).")
            e.scheduled_at = at
        return {"object": "email", "id": e.id}

    def _cancel_email(self, email_id: str) -> dict[str, Any]:
        e = self._email(email_id)
        with self.mock.state.lock:
            if e.last_event != "scheduled" or e.dispatched:
                raise ApiErr(422, "validation_error",
                             "Email cannot be canceled because it is not scheduled (already sent or canceled).")
            e.last_event = "canceled"
            e.events.append({"type": "email.canceled", "at": time.time(), "svix_id": None,
                             "webhook": "none"})
        return {"object": "email", "id": e.id}

    def _domains(self, domain_id: str | None) -> dict[str, Any]:
        domains = sorted(self.mock.state.local_domains())

        def row(name: str) -> dict[str, Any]:
            return {"id": str(uuid.uuid5(uuid.NAMESPACE_DNS, name)), "name": name,
                    "status": "verified", "created_at": "2026-01-01 00:00:00.000000+00",
                    "region": "us-east-1",
                    "capabilities": {"sending": "enabled", "receiving": "enabled"}}

        if domain_id is None:
            return {"object": "list", "has_more": False, "data": [row(d) for d in domains]}
        for d in domains:
            if row(d)["id"] == domain_id:
                out = row(d)
                out["object"] = "domain"
                out["records"] = [
                    {"record": "SPF", "name": "send", "type": "MX", "ttl": "Auto", "status": "verified",
                     "value": "feedback-smtp.us-east-1.amazonses.com", "priority": 10},
                    {"record": "SPF", "name": "send", "type": "TXT", "ttl": "Auto", "status": "verified",
                     "value": "\"v=spf1 include:amazonses.com ~all\""},
                    {"record": "DKIM", "name": "resend._domainkey", "type": "TXT", "ttl": "Auto",
                     "status": "verified", "value": "p=MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDmock"},
                    {"record": "Receiving", "name": "@", "type": "MX", "ttl": "Auto",
                     "status": "verified", "value": "inbound-smtp.us-east-1.amazonaws.com",
                     "priority": 10},
                ]
                return out
        raise ApiErr(404, "not_found", "Domain not found")

    # -- receiving ------------------------------------------------------------------------------
    def _rec(self, rid: str) -> ReceivedEmail:
        with self.mock.state.lock:
            rec = self.mock.state.received_by_id.get(rid)
        if rec is None:
            raise ApiErr(404, "not_found", "Email not found")
        return rec

    def _receiving(self, rest: list[str], query: dict[str, str]) -> dict[str, Any]:
        host = self.headers.get("Host") or "127.0.0.1"
        if not rest:
            if "limit" not in query:
                raise ApiErr(422, "validation_error",
                             "mock: `limit` (1-100) is required (AZ Mail always pages).")
            limit = _limit(query.get("limit"), default=20)
            with self.mock.state.lock:
                items = list(reversed(self.mock.state.received))
            sel, more = _page(items, limit, query.get("after"), query.get("before"), lambda r: r.id)
            return {"object": "list", "has_more": more, "data": [
                {"id": r.id, "to": r.to, "from": r.from_, "created_at": iso_z(r.created_at),
                 "subject": r.subject, "bcc": r.bcc, "cc": r.cc, "reply_to": r.reply_to,
                 "message_id": r.message_id,
                 "attachments": [{"filename": a.filename, "content_type": a.content_type,
                                  "content_id": a.content_id, "content_disposition": a.disposition,
                                  "id": a.id, "size": a.size} for a in r.attachments]}
                for r in sel]}
        rec = self._rec(rest[0])
        if len(rest) == 1:
            fmt = query.get("html_format", "data_uri")
            if fmt not in ("cid", "data_uri"):
                raise ApiErr(422, "validation_error", "`html_format` must be cid or data_uri.")
            html = rec.html
            if html is not None and fmt == "data_uri":
                for a in rec.attachments:
                    if a.content_id:
                        uri = f"data:{a.content_type};base64,{base64.b64encode(a.data).decode()}"
                        html = html.replace(f"cid:{a.content_id}", uri)
            raw = None
            if not self.mock.cfg["raw_missing"]:
                url, exp = self.mock.new_download("raw", rec, None, host)
                raw = {"download_url": url, "expires_at": iso_z(exp)}
            return {
                "object": "email", "id": rec.id, "to": rec.to, "from": rec.from_,
                "created_at": iso_z(rec.created_at), "subject": rec.subject, "html": html,
                "html_format": fmt, "text": rec.text, "headers": rec.headers, "bcc": rec.bcc,
                "cc": rec.cc, "reply_to": rec.reply_to, "received_for": rec.received_for,
                "authentication": rec.authentication, "message_id": rec.message_id, "raw": raw,
                "attachments": [{"id": a.id, "filename": a.filename, "content_type": a.content_type,
                                 "content_disposition": a.disposition, "content_id": a.content_id,
                                 "size": a.size} for a in rec.attachments],
            }
        if rest[1] != "attachments" or len(rest) > 3:
            raise ApiErr(404, "not_found", "The requested endpoint does not exist.")

        def att_json(a: Attachment) -> dict[str, Any]:
            url, exp = self.mock.new_download("attachment", rec, a, host)
            return {"id": a.id, "filename": a.filename, "size": a.size,
                    "content_type": a.content_type, "content_disposition": a.disposition,
                    "content_id": a.content_id, "download_url": url, "expires_at": iso_z(exp)}

        if len(rest) == 3:
            for a in rec.attachments:
                if a.id == rest[2]:
                    out = att_json(a)
                    out["object"] = "attachment"
                    return out
            raise ApiErr(404, "not_found", "Attachment not found")
        limit = _limit(query.get("limit"), default=20)
        sel, more = _page(rec.attachments, limit, query.get("after"), query.get("before"), lambda a: a.id)
        return {"object": "list", "has_more": more, "data": [att_json(a) for a in sel]}

    # -- download URLs --------------------------------------------------------------------------
    def _download(self, method: str, path: str, query: dict[str, str], final: bool) -> None:
        st = self.mock.state
        if method not in ("GET", "HEAD"):
            self._json(405, {"error": "method_not_allowed"})
            return
        if self.headers.get("Authorization"):
            with st.lock:
                st.violations.append({"at": time.time(), "path": path.split("/")[1],
                                      "reason": "Authorization header sent to a download URL",
                                      "user_agent": self.headers.get("User-Agent", "")})
            self._json(400, {"error": "authorization_header_forbidden",
                             "message": "Download URLs are pre-signed; never send credentials."},
                       method=method)
            return
        token = path.split("/", 2)[2] if path.count("/") >= 2 else ""
        with st.lock:
            info = st.downloads.get(token)
        if info is None:
            self._json(404, {"error": "not_found"}, method=method)
            return
        try:
            exp = int(query.get("exp", ""))
        except ValueError:
            exp = -1
        if exp != info["exp"] or time.time() > info["exp"]:
            self._json(403, {"error": "expired", "message": "This download URL has expired."},
                       method=method)
            return
        host = self.headers.get("Host") or "127.0.0.1"
        if not final:
            self._send(302, b"", "text/plain", {"Location": f"http://{host}/_blob/{token}?exp={exp}"},
                       method=method)
            return
        with st.lock:
            rec = st.received_by_id.get(info["received_id"])
        if rec is None:
            self._json(404, {"error": "not_found"}, method=method)
            return
        if info["kind"] == "raw":
            data, ctype, filename = rec.raw, "message/rfc822", f"{rec.id}.eml"
        else:
            att = next((a for a in rec.attachments if a.id == info["att_id"]), None)
            if att is None:
                self._json(404, {"error": "not_found"}, method=method)
                return
            data, ctype, filename = att.data, att.content_type, att.filename
        disp = "attachment; filename*=UTF-8''" + urllib.parse.quote(filename, safe="")
        self._send(200, data, ctype, {"Content-Disposition": disp}, method=method)

    # -- control endpoints ----------------------------------------------------------------------
    def _control(self, method: str, path: str, query: dict[str, str], body: bytes) -> None:
        try:
            data = json.loads(body) if body else None
        except ValueError:
            self._json(400, {"error": "invalid_json"})
            return
        parts = [p for p in path.split("/") if p][1:]  # after "_mock"
        try:
            result = self._control_route(method, parts, query, data)
        except ApiErr as err:
            self._json(err.status, {"error": err.name, "message": err.message})
            return
        except (ValueError, TypeError, KeyError) as err:
            self._json(400, {"error": "bad_request", "message": str(err)})
            return
        if isinstance(result, tuple):
            status, ctype, payload = result
            self._send(status, payload, ctype, method=method)
            return
        self._json(200, result, method=method)

    def _control_route(self, method: str, parts: list[str], query: dict[str, str], data: Any) -> Any:
        mock = self.mock
        st = mock.state
        since = int(query.get("since", "0") or 0)
        if parts == ["health"]:
            return {"ok": True}
        if parts == ["state"]:
            with st.lock:
                return {"seq": st.last_seq, "now_ms": int(st.now() * 1000), "offset": st.offset,
                        "config": dict(st.config), "emails": len(st.emails),
                        "received": len(st.received), "pending_webhooks": mock.sender.pending(),
                        "idle": mock.idle(), "faults": [f.to_json() for f in st.faults],
                        "violations": len(st.violations), "s3_objects": len(st.objects)}
        if parts == ["config"]:
            if method == "POST":
                if not isinstance(data, dict):
                    raise ApiErr(400, "bad_request", "config body must be an object")
                return st.update_config(data)
            return dict(st.config)
        if parts == ["faults"]:
            if method == "POST":
                items = data if isinstance(data, list) else (data or {}).get("faults", [])
                append = isinstance(data, dict) and bool(data.get("append"))
                faults = [Fault.from_json(f) for f in items]
                with st.lock:
                    if not append:
                        st.faults.clear()
                    st.faults.extend(faults)
            with st.lock:
                return {"faults": [f.to_json() for f in st.faults]}
        if parts == ["advance"] and method == "POST":
            seconds = float(query.get("seconds") or (data or {}).get("seconds") or 0)
            if seconds < 0:
                raise ApiErr(400, "bad_request", "seconds must be >= 0")
            with mock._fire_lock:
                with st.lock:
                    st.offset += seconds
                fired = mock.fire_due()
            return {"now_ms": int(st.now() * 1000), "offset": st.offset, "fired": fired}
        if parts == ["sent"]:
            path_filter = query.get("path")
            method_filter = query.get("method")
            with st.lock:
                reqs = [dict(r) for r in st.requests if r["seq"] > since
                        and (not path_filter or r["path"] == path_filter)
                        and (not method_filter or r["method"] == method_filter.upper())]
                emails = [e.summary() for e in sorted(st.emails.values(), key=lambda e: e.seq)
                          if e.seq > since]
                return {"seq": st.last_seq, "requests": reqs, "emails": emails}
        if len(parts) == 2 and parts[0] == "emails":
            with st.lock:
                e = st.emails.get(parts[1])
                if e is None:
                    raise ApiErr(404, "not_found", "unknown email id")
                return e.summary()
        if parts and parts[0] == "received":
            if len(parts) == 1:
                with st.lock:
                    return {"seq": st.last_seq,
                            "received": [r.summary() for r in st.received if r.seq > since]}
            with st.lock:
                rec = st.received_by_id.get(parts[1])
            if rec is None:
                raise ApiErr(404, "not_found", "unknown received id")
            if len(parts) == 3 and parts[2] == "raw":
                return 200, "message/rfc822", rec.raw
            return rec.summary(with_raw=query.get("raw") == "1")
        if parts == ["inbound"] and method == "POST":
            return mock.inject(data or {})
        if parts == ["webhook"] and method == "POST":
            return self._manual_webhook(data or {})
        if parts == ["webhooks"]:
            if query.get("wait") == "1":
                mock.sender.wait_idle(float(query.get("timeout", "10")))
            return {"pending": mock.sender.pending(),
                    "deliveries": [d for d in list(mock.sender.log) if d["seq"] > since]}
        if parts == ["violations"]:
            with st.lock:
                return {"violations": list(st.violations)}
        if parts == ["s3"]:
            with st.lock:
                objs = [{"bucket": b, "key": k, "size": len(o.data), "sha256": o.sha256,
                         "content_type": o.content_type, "etag": o.etag}
                        for (b, k), o in sorted(st.objects.items())]
                reqs = [dict(r) for r in st.s3_requests if r["seq"] > since]
                return {"seq": st.last_seq, "buckets": sorted(st.buckets), "objects": objs,
                        "requests": reqs}
        if parts == ["reset"] and method == "POST":
            opts = data if isinstance(data, dict) else {}
            keep = bool(opts.get("keep_data", False))
            st.reset(keep_data=keep, wipe_s3=bool(opts.get("s3", False)))
            if opts.get("s3") and mock.s3_disk is not None:
                mock.s3_disk.clear()
            if not keep:
                mock.sender.clear()
                mock.scheduler.clear()
            return {"ok": True, "seq": st.last_seq}
        raise ApiErr(404, "not_found", f"unknown control endpoint {method} /_mock/{'/'.join(parts)}")

    def _manual_webhook(self, data: dict[str, Any]) -> dict[str, Any]:
        """POST /_mock/webhook: {email_id[, type]} re-emits an event; {payload} sends it verbatim."""
        mock = self.mock
        if isinstance(data.get("payload"), dict):
            sid = mock._send_webhook(data["payload"], data.get("svix_id"))
            return {"svix_id": sid}
        eid = data.get("email_id")
        if not isinstance(eid, str):
            raise ApiErr(400, "bad_request", "email_id or payload required")
        with mock.state.lock:
            rec = mock.state.received_by_id.get(eid)
            sent = mock.state.emails.get(eid)
        if rec is not None:
            return {"svix_id": mock.notify_received(rec, data.get("svix_id"))}
        if sent is not None:
            etype = data.get("type") or "email.delivered"
            return {"svix_id": mock._emit(sent, etype, data.get("svix_id"))}
        raise ApiErr(404, "not_found", "unknown email id")


def _limit(value: str | None, default: int) -> int:
    if value is None or value == "":
        return default
    try:
        n = int(value)
    except ValueError:
        raise ApiErr(422, "validation_error", "`limit` must be an integer between 1 and 100.") from None
    if not 1 <= n <= 100:
        raise ApiErr(422, "validation_error", "`limit` must be an integer between 1 and 100.")
    return n


def _page(items: list[Any], limit: int, after: str | None, before: str | None,
          id_of: Callable[[Any], str]) -> tuple[list[Any], bool]:
    """Cursor paging over a newest-first list: ``after`` = older items, ``before`` = newer items."""
    if after and before:
        raise ApiErr(422, "validation_error", "`after` and `before` cannot be used together.")
    ids = [id_of(x) for x in items]
    if after:
        if after not in ids:
            raise ApiErr(422, "validation_error", "Invalid `after` cursor.")
        start = ids.index(after) + 1
        return items[start:start + limit], len(items) > start + limit
    if before:
        if before not in ids:
            raise ApiErr(422, "validation_error", "Invalid `before` cursor.")
        end = ids.index(before)
        start = max(0, end - limit)
        return items[start:end], start > 0
    return items[:limit], len(items) > limit


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="mock_resend", description="Mock Resend API + minimal R2 (S3)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787, help="API port (0 = random)")
    ap.add_argument("--api-key", default=os.environ.get("MOCK_RESEND_API_KEY", DEFAULT_API_KEY))
    ap.add_argument("--webhook-url", default="", help="where signed webhooks are POSTed")
    ap.add_argument("--webhook-secret", default=DEFAULT_WEBHOOK_SECRET, help="whsec_<base64>")
    ap.add_argument("--local-domains", default="", help="comma list of team domains (loopback)")
    ap.add_argument("--time-scale", type=float, default=1.0,
                    help="multiplier for internal delays (event spacing, webhook retries)")
    ap.add_argument("--meta-delay", type=float, default=0.0,
                    help="seconds until GET /emails/{id}.message_id is non-null")
    ap.add_argument("--rate-limit", type=int, default=10, help="requests/second (0 = off)")
    ap.add_argument("--reject-scheduled-attachments", action="store_true")
    ap.add_argument("--s3-port", type=int, default=-1, help="R2/S3 port (0 = random, -1 = off)")
    ap.add_argument("--s3-access-key", default=DEFAULT_S3_ACCESS_KEY)
    ap.add_argument("--s3-secret-key", default=DEFAULT_S3_SECRET_KEY)
    ap.add_argument("--s3-bucket", default="azmail", help="comma list of existing buckets")
    ap.add_argument("--s3-regions", default="auto", help="accepted SigV4 regions (comma list)")
    ap.add_argument("--s3-write-interval", type=float, default=1.0,
                    help="min seconds between writes of one key (R2: 1 write/s)")
    ap.add_argument("--s3-max-object-mb", type=int, default=200)
    ap.add_argument("--s3-data-dir", default="", help="persist R2 objects here (dev); default in memory")
    ap.add_argument("--port-file", default="", help="write {port, s3_port, pid} JSON here when ready")
    ap.add_argument("--verbose", action="store_true")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    mock = MockResend(
        api_key=args.api_key, webhook_url=args.webhook_url, webhook_secret=args.webhook_secret,
        local_domains=[d for d in args.local_domains.split(",") if d.strip()],
        time_scale=args.time_scale, meta_delay=args.meta_delay, rate_limit=args.rate_limit,
        reject_scheduled_attachments=args.reject_scheduled_attachments,
        s3_access_key=args.s3_access_key, s3_secret_key=args.s3_secret_key,
        s3_buckets=[b.strip() for b in args.s3_bucket.split(",") if b.strip()],
        s3_regions=[r.strip() for r in args.s3_regions.split(",") if r.strip()],
        s3_write_interval=args.s3_write_interval, s3_max_object_mb=args.s3_max_object_mb,
        s3_data_dir=args.s3_data_dir or None, verbose=args.verbose)
    port, s3_port = mock.serve(args.host, args.port, args.s3_port if args.s3_port >= 0 else None)
    if args.port_file:
        tmp = args.port_file + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump({"port": port, "s3_port": s3_port, "pid": os.getpid()}, fh)
        os.replace(tmp, args.port_file)
    print(f"mock_resend: API http://{args.host}:{port}"
          + (f"  R2/S3 http://{args.host}:{s3_port}" if s3_port else "")
          + f"  webhooks -> {args.webhook_url or '(none)'}", flush=True)
    stop = threading.Event()

    def _on_signal(signum: int, _frame: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)
    while not stop.wait(0.5):
        pass
    mock.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
