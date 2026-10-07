"""Minimal RFC 6455 WebSocket client (text frames, ping/pong, close codes) for E2E tests."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import queue
import socket
import struct
import threading
import time
import urllib.parse
from typing import Any, Callable

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WsHandshakeError(AssertionError):
    def __init__(self, status: int, headers: dict[str, str], body: bytes) -> None:
        self.status = status
        self.headers = headers
        self.body = body
        super().__init__(f"WebSocket handshake rejected: HTTP {status} {body[:200]!r}")


class WsClient:
    def __init__(self, url: str, origin: str | None = None, timeout: float = 10.0,
                 extra_headers: dict[str, str] | None = None) -> None:
        self.url = url
        self.origin = origin
        self.timeout = timeout
        self.extra_headers = dict(extra_headers or {})
        self.sock: socket.socket | None = None
        self.messages: list[Any] = []  # every message received, in order
        self._queue: "queue.Queue[Any]" = queue.Queue()
        self.closed = threading.Event()
        self.close_code: int | None = None
        self.close_reason = ""
        self._send_lock = threading.Lock()
        self._reader: threading.Thread | None = None
        self._consumed: set[int] = set()  # ids of messages already returned by wait_for
        self._buffer = b""

    # -- connection -----------------------------------------------------------------------------
    def connect(self) -> "WsClient":
        parts = urllib.parse.urlsplit(self.url)
        host, port = parts.hostname or "127.0.0.1", parts.port or 80
        sock = socket.create_connection((host, port), timeout=self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [f"GET {parts.path or '/'}{'?' + parts.query if parts.query else ''} HTTP/1.1",
                 f"Host: {host}:{port}", "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13",
                 "User-Agent: azmail-e2e-ws/1.0"]
        if self.origin is not None:
            lines.append(f"Origin: {self.origin}")
        lines += [f"{k}: {v}" for k, v in self.extra_headers.items()]
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(4096)
            if not chunk:
                break
            head += chunk
        raw_head, _, rest = head.partition(b"\r\n\r\n")
        text = raw_head.decode("latin-1").split("\r\n")
        try:
            status = int(text[0].split()[1])
        except (IndexError, ValueError):
            sock.close()
            raise WsHandshakeError(0, {}, head) from None
        headers = {}
        for line in text[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        if status != 101:
            body = rest
            try:
                length = int(headers.get("content-length", "0"))
                while len(body) < length:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    body += chunk
            except (OSError, ValueError):
                pass
            sock.close()
            raise WsHandshakeError(status, headers, body)
        expected = base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest()).decode()  # noqa: S324
        if headers.get("sec-websocket-accept") != expected:
            sock.close()
            raise AssertionError("WebSocket handshake: bad Sec-WebSocket-Accept")
        sock.settimeout(None)
        self.sock = sock
        self._buffer = rest
        self._reader = threading.Thread(target=self._read_loop, name="ws-reader", daemon=True)
        self._reader.start()
        return self

    def auth(self, token: str, timeout: float = 10.0) -> dict[str, Any]:
        """Sends the first-message auth and waits for ``ready``."""
        self.send_json({"type": "auth", "token": token})
        return self.wait_for("ready", timeout)

    def close(self, code: int = 1000) -> None:
        if self.sock is None:
            return
        if not self.closed.is_set():
            try:
                self._send_frame(0x8, struct.pack("!H", code))
            except OSError:
                pass
            self.closed.wait(2)
        try:
            self.sock.close()
        except OSError:
            pass

    # -- sending --------------------------------------------------------------------------------
    def send_json(self, obj: Any) -> None:
        self._send_frame(0x1, json.dumps(obj).encode())

    def send_text(self, text: str) -> None:
        self._send_frame(0x1, text.encode())

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        assert self.sock is not None, "not connected"
        header = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", n)
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self._send_lock:
            self.sock.sendall(bytes(header) + mask + masked)

    # -- receiving ------------------------------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        while len(self._buffer) < n:
            assert self.sock is not None
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self._buffer += chunk
        out, self._buffer = self._buffer[:n], self._buffer[n:]
        return out

    def _read_loop(self) -> None:
        fragments = b""
        try:
            while True:
                b1, b2 = self._recv_exact(2)
                fin, opcode = b1 & 0x80, b1 & 0x0F
                n = b2 & 0x7F
                if n == 126:
                    n = struct.unpack("!H", self._recv_exact(2))[0]
                elif n == 127:
                    n = struct.unpack("!Q", self._recv_exact(8))[0]
                mask = self._recv_exact(4) if b2 & 0x80 else None
                payload = self._recv_exact(n)
                if mask:
                    payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                if opcode == 0x9:  # ping
                    self._send_frame(0xA, payload)
                    continue
                if opcode == 0xA:  # pong
                    continue
                if opcode == 0x8:  # close
                    self.close_code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 1005
                    self.close_reason = payload[2:].decode("utf-8", "replace")
                    try:
                        self._send_frame(0x8, payload[:2])
                    except OSError:
                        pass
                    break
                fragments += payload
                if not fin:
                    continue
                data, fragments = fragments, b""
                try:
                    msg: Any = json.loads(data)
                except ValueError:
                    msg = data.decode("utf-8", "replace")
                self.messages.append(msg)
                self._queue.put(msg)
        except (OSError, ConnectionError, AssertionError, struct.error, ValueError):
            if self.close_code is None:
                self.close_code = 1006
        finally:
            self.closed.set()

    def recv(self, timeout: float = 5.0) -> Any:
        try:
            return self._queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def wait_for(self, match: str | Callable[[Any], bool], timeout: float = 10.0) -> Any:
        """Waits for a message whose ``type`` equals ``match`` (or for which ``match(msg)`` is true).

        Messages already received are searched first (in order); a message is returned at most
        once per call.
        """
        pred = (lambda m: isinstance(m, dict) and m.get("type") == match) if isinstance(match, str) else match
        for msg in list(self.messages):
            if pred(msg) and id(msg) not in self._consumed:
                self._consumed.add(id(msg))
                return msg
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                types = [m.get("type") if isinstance(m, dict) else m for m in self.messages]
                raise AssertionError(f"WebSocket: no message matching {match!r} within {timeout:.0f}s "
                                     f"(received: {types}; closed={self.closed.is_set()} code={self.close_code})")
            msg = self.recv(min(left, 0.25))
            if msg is not None and pred(msg) and id(msg) not in self._consumed:
                self._consumed.add(id(msg))
                return msg

    def wait_closed(self, timeout: float = 10.0) -> int | None:
        if not self.closed.wait(timeout):
            raise AssertionError(f"WebSocket still open after {timeout:.0f}s")
        return self.close_code
