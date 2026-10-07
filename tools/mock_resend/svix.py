"""Svix webhook signing (what Resend uses) and a retrying webhook sender.

Signed content = ``"<svix-id>.<svix-timestamp>.<raw body>"``; HMAC-SHA256 keyed with the base64
secret after the ``whsec_`` prefix; header ``svix-signature: v1,<base64>`` (space-separated list).
"""

from __future__ import annotations

import base64
import hashlib
import heapq
import hmac
import http.client
import itertools
import json
import secrets
import string
import threading
import time
import urllib.parse
from collections import deque
from typing import Any, Callable

SVIX_USER_AGENT = "Svix-Webhooks/1.0 (mock_resend; +https://www.svix.com/http-sender/)"
RETRY_DELAYS = (0.0, 1.0, 3.0)  # first attempt, then retries (scaled by --time-scale)


def _secret_bytes(secret: str) -> bytes:
    if not secret.startswith("whsec_"):
        raise ValueError("webhook secret must start with whsec_")
    try:
        return base64.b64decode(secret[len("whsec_"):], validate=True)
    except ValueError as exc:  # binascii.Error is a ValueError
        raise ValueError("webhook secret is not valid base64") from exc


def sign(secret: str, msg_id: str, timestamp: int | str, body: bytes | str) -> str:
    """``v1,<base64 HMAC>`` for the given inputs."""
    payload = body.encode("utf-8") if isinstance(body, str) else body
    content = f"{msg_id}.{timestamp}.".encode("utf-8") + payload
    mac = hmac.new(_secret_bytes(secret), content, hashlib.sha256).digest()
    return "v1," + base64.b64encode(mac).decode("ascii")


def verify(secret: str, msg_id: str, timestamp: str, sig_header: str, body: bytes | str,
           now: float | None = None, tolerance: int = 300) -> bool:
    """True when any v1 signature in ``sig_header`` matches and the timestamp is fresh."""
    if not msg_id or not timestamp or not sig_header:
        return False
    try:
        ts = int(timestamp)
    except ValueError:
        return False
    current = time.time() if now is None else now
    if abs(current - ts) > tolerance:
        return False
    expected = sign(secret, msg_id, timestamp, body).split(",", 1)[1]
    for entry in sig_header.split():
        version, _, sig = entry.partition(",")
        if version == "v1" and hmac.compare_digest(sig, expected):
            return True
    return False


_ALPHABET = string.ascii_letters + string.digits


def new_msg_id() -> str:
    return "msg_" + "".join(secrets.choice(_ALPHABET) for _ in range(27))


def new_secret() -> str:
    return "whsec_" + base64.b64encode(secrets.token_bytes(24)).decode("ascii")


def encode_body(payload: dict[str, Any]) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


class WebhookSender:
    """Delivers signed webhooks from one worker thread (keeps per-email event order).

    Every delivery is attempted at the RETRY_DELAYS offsets (each attempt re-signed with a fresh
    timestamp, same svix-id) until a 2xx answer. ``get_target()`` returns (url, secret) at send
    time so /_mock/config changes apply immediately. Every attempt is logged.
    """

    def __init__(self, get_target: Callable[[], tuple[str, str]], time_scale: float = 1.0,
                 log_size: int = 5000) -> None:
        self._get_target = get_target
        self._scale = max(0.0, float(time_scale))
        self._heap: list[tuple[float, int, dict[str, Any]]] = []
        self._cv = threading.Condition()
        self._seq = itertools.count(1)
        self._stop = False
        self.log: deque[dict[str, Any]] = deque(maxlen=log_size)
        self._log_seq = itertools.count(1)
        self._pending = 0
        self._thread = threading.Thread(target=self._run, name="webhook-sender", daemon=True)
        self._thread.start()

    # -- public ---------------------------------------------------------------------------------
    def enqueue(self, payload: dict[str, Any], *, svix_id: str | None = None,
                delay: float = 0.0, copies: int = 1) -> str:
        """Queues ``copies`` deliveries of ``payload`` (same svix-id) after ``delay`` seconds."""
        msg_id = svix_id or new_msg_id()
        body = encode_body(payload)
        with self._cv:
            for i in range(max(1, copies)):
                job = {"svix_id": msg_id, "body": body, "type": payload.get("type"),
                       "email_id": (payload.get("data") or {}).get("email_id"),
                       "attempt": 0, "copy": i + 1}
                due = time.time() + delay * self._scale + i * 0.01
                heapq.heappush(self._heap, (due, next(self._seq), job))
                self._pending += 1
            self._cv.notify_all()
        return msg_id

    def pending(self) -> int:
        with self._cv:
            return self._pending

    def wait_idle(self, timeout: float = 10.0) -> bool:
        deadline = time.time() + timeout
        with self._cv:
            while self._pending:
                left = deadline - time.time()
                if left <= 0:
                    return False
                self._cv.wait(min(left, 0.05))
        return True

    def clear(self) -> None:
        with self._cv:
            self._heap.clear()
            self._pending = 0
            self.log.clear()
            self._cv.notify_all()

    def stop(self) -> None:
        with self._cv:
            self._stop = True
            self._cv.notify_all()
        self._thread.join(timeout=5)

    # -- worker ---------------------------------------------------------------------------------
    def _run(self) -> None:
        while True:
            with self._cv:
                while not self._stop and (not self._heap or self._heap[0][0] > time.time()):
                    timeout = None if not self._heap else max(0.0, self._heap[0][0] - time.time())
                    self._cv.wait(timeout if timeout is not None else 0.5)
                if self._stop:
                    return
                _, _, job = heapq.heappop(self._heap)
            ok = self._deliver(job)
            with self._cv:
                job["attempt"] += 1
                if not ok and job["attempt"] < len(RETRY_DELAYS):
                    due = time.time() + RETRY_DELAYS[job["attempt"]] * self._scale
                    heapq.heappush(self._heap, (due, next(self._seq), job))
                else:
                    self._pending -= 1
                self._cv.notify_all()

    def _deliver(self, job: dict[str, Any]) -> bool:
        url, secret = self._get_target()
        entry: dict[str, Any] = {"seq": next(self._log_seq), "at": time.time(),
                                 "svix_id": job["svix_id"], "type": job["type"],
                                 "email_id": job["email_id"], "attempt": job["attempt"] + 1,
                                 "copy": job["copy"], "status": None, "error": None}
        self.log.append(entry)
        if not url:
            entry["error"] = "no webhook_url configured"
            return True  # nothing to retry against
        ts = str(int(time.time()))
        headers = {
            "Content-Type": "application/json",
            "User-Agent": SVIX_USER_AGENT,
            "svix-id": job["svix_id"],
            "svix-timestamp": ts,
            "svix-signature": sign(secret, job["svix_id"], ts, job["body"]),
        }
        parsed = urllib.parse.urlsplit(url)
        conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
        try:
            conn = conn_cls(parsed.hostname, parsed.port, timeout=15)
            try:
                path = parsed.path or "/"
                if parsed.query:
                    path += "?" + parsed.query
                conn.request("POST", path, body=job["body"], headers=headers)
                resp = conn.getresponse()
                resp.read()
                entry["status"] = resp.status
                return 200 <= resp.status < 300
            finally:
                conn.close()
        except OSError as exc:
            entry["error"] = f"{type(exc).__name__}: {exc}"
            return False
