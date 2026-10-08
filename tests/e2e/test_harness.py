#!/usr/bin/env python3
"""Hermetic unit tests of the E2E harness itself (no azmail binary needed).

    python3 tests/e2e/test_harness.py [-v]

Covers lib/ws.py against an in-test RFC 6455 server (handshake, masking, fragments, ping/pong,
close codes), lib/api.py + lib/mock.py against an in-process mock, lib/signing.py, lib/wait.py,
lib/proc.py and the run.py CLI (--list, missing binary).
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(REPO))

from lib import signing  # noqa: E402
from lib.api import Api, ApiError, http_request, raw_request  # noqa: E402
from lib.mock import MockClient  # noqa: E402
from lib.proc import Proc, free_port, run_cli, tail_file  # noqa: E402
from lib.wait import eventually, never, wait_for  # noqa: E402
from lib.ws import WsClient, WsHandshakeError  # noqa: E402
from tools.mock_resend.server import MockResend  # noqa: E402

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# ---------------------------------------------------------------------------------------------
# a tiny WebSocket server mimicking the AZ Mail protocol
# ---------------------------------------------------------------------------------------------

class _WsHandler(socketserver.BaseRequestHandler):
    allowed_origin = "http://localhost:5173"

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def _send(self, opcode: int, payload: bytes, fin: bool = True) -> None:
        head = bytes([(0x80 if fin else 0) | opcode])
        n = len(payload)
        head += bytes([n]) if n < 126 else bytes([126]) + struct.pack("!H", n)
        self.request.sendall(head + payload)

    def _recv_frame(self) -> tuple[int, bytes]:
        b1, b2 = self._recv_exact(2)
        assert b2 & 0x80, "client frames must be masked"
        n = b2 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._recv_exact(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4)
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(self._recv_exact(n)))
        return b1 & 0x0F, data

    def handle(self) -> None:
        req = b""
        while b"\r\n\r\n" not in req:
            req += self.request.recv(4096)
        lines = req.decode().split("\r\n")
        headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        if headers.get("origin") != self.allowed_origin:
            body = b'{"error":{"code":"forbidden"}}'
            self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body))
            return
        accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + GUID).encode()).digest())  # noqa: S324
        self.request.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        self.request.settimeout(10)
        try:
            op, data = self._recv_frame()
            msg = json.loads(data)
            if msg.get("type") != "auth" or msg.get("token") != "good":
                self._send(0x8, struct.pack("!H", 4401) + b"auth failed")
                return
            self._send(0x1, json.dumps({"type": "ready", "user_id": 7, "server_time": 1}).encode())
            self._send(0x9, b"hb")  # server ping: the client must pong
            op, data = self._recv_frame()
            if op == 0x8:  # the client closed before answering
                self._send(0x8, data[:2])
                return
            assert op == 0xA and data == b"hb", (op, data)
            part = json.dumps({"type": "mail.new", "subject": "分片 " + "x" * 300}).encode()
            self._send(0x1, part[:100], fin=False)
            self._send(0x0, part[100:])
            while True:
                op, data = self._recv_frame()
                if op == 0x8:
                    self._send(0x8, data[:2])
                    return
                if json.loads(data).get("type") == "ping":
                    self._send(0x1, b'{"type":"pong"}')
                if json.loads(data).get("type") == "revoke":
                    self._send(0x1, b'{"type":"session.revoked"}')
                    self._send(0x8, struct.pack("!H", 4401))
                    return
        except (ConnectionError, OSError, AssertionError):
            return


class WsClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), _WsHandler)
        cls.server.daemon_threads = True
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.url = f"ws://127.0.0.1:{cls.server.server_address[1]}/api/ws"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()

    def test_bad_origin_rejected(self) -> None:
        for origin in ("http://evil.test", None):
            with self.assertRaises(WsHandshakeError) as ctx:
                WsClient(self.url, origin=origin).connect()
            self.assertEqual(ctx.exception.status, 403)
            self.assertIn(b"forbidden", ctx.exception.body)

    def test_auth_ping_fragments_pong_revoke(self) -> None:
        ws = WsClient(self.url, origin="http://localhost:5173").connect()
        ready = ws.auth("good")
        self.assertEqual(ready["user_id"], 7)
        msg = ws.wait_for("mail.new", 5)
        self.assertTrue(msg["subject"].startswith("分片 "))
        self.assertEqual(len(msg["subject"]), 303)
        ws.send_json({"type": "ping"})
        self.assertEqual(ws.wait_for("pong", 5), {"type": "pong"})
        with self.assertRaises(AssertionError):
            ws.wait_for("pong", 0.3)  # already consumed: returned at most once
        ws.send_json({"type": "revoke"})
        self.assertEqual(ws.wait_closed(5), 4401)
        self.assertTrue(any(m.get("type") == "session.revoked" for m in ws.messages))
        ws.close()

    def test_bad_token_closes_4401(self) -> None:
        ws = WsClient(self.url, origin="http://localhost:5173").connect()
        ws.send_json({"type": "auth", "token": "bad"})
        self.assertEqual(ws.wait_closed(5), 4401)
        self.assertEqual(ws.close_reason, "auth failed")

    def test_clean_close(self) -> None:
        ws = WsClient(self.url, origin="http://localhost:5173").connect()
        ws.auth("good")
        ws.close(1000)
        self.assertEqual(ws.close_code, 1000)


# ---------------------------------------------------------------------------------------------
# REST client + mock client against an in-process mock
# ---------------------------------------------------------------------------------------------

class ApiAndMockClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.mock = MockResend(api_key="re_k", local_domains=["corp.test"], time_scale=0.05, rate_limit=0)
        port, s3 = cls.mock.serve("127.0.0.1", 0, 0)
        cls.base = f"http://127.0.0.1:{port}"
        cls.client = MockClient(cls.base, "re_k", f"http://127.0.0.1:{s3}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.mock.shutdown()

    def test_api_expect_and_errors(self) -> None:
        api = Api(self.base)
        self.assertEqual(api.get("/_mock/health"), {"ok": True})
        with self.assertRaises(ApiError) as ctx:
            api.get("/_mock/nope")
        self.assertIn("404", str(ctx.exception))
        self.assertEqual(ctx.exception.resp.status, 404)
        r = api.call("GET", "/emails/x")  # no auth → 401 JSON error from the mock
        self.assertEqual(r.status, 401)
        self.assertEqual(r.json()["name"], "missing_api_key")
        err = ApiError("GET", "http://h/api/files/1?d=a&u=1&exp=2&sig=SECRET", r, [200])
        self.assertNotIn("SECRET", str(err))

    def test_no_redirect_following_and_inject(self) -> None:
        since = self.client.seq()
        out = self.client.inbound(from_="p@ext.test", to="alice@corp.test", subject="附件",
                                  attachments=[{"filename": "a.bin", "data": b"\x00\x01"}])
        rec = self.client.received(since)
        self.assertEqual([r["id"] for r in rec], out["ids"])
        self.assertEqual(rec[0]["attachments"][0]["size"], 2)
        raw = self.client.received_raw(out["ids"][0])
        # the MX trace header first (single recipient → FOR clause), then the message as built
        head, _, rest = raw.partition(b"\r\nAuthentication-Results:")
        self.assertTrue(head.startswith(b"Received: from mail.ext.test"), raw[:120])
        self.assertIn(b"for <alice@corp.test>;", head.replace(b"\r\n        ", b" "))
        self.assertTrue(rest, "Authentication-Results must follow the Received header")
        self.assertEqual(rec[0]["raw_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(out["raw_sha256"], rec[0]["raw_sha256"])
        atts = http_request("GET", f"{self.base}/emails/receiving/{out['ids'][0]}/attachments",
                            headers={"Authorization": "Bearer re_k"})
        url = json.loads(atts.body)["data"][0]["download_url"]
        r = http_request("GET", url)
        self.assertEqual(r.status, 302)  # not followed
        self.assertIn("/_blob/", r.header("location"))
        self.assertEqual(http_request("GET", r.header("location")).body, b"\x00\x01")
        self.assertEqual(self.client.violations(), [])

    def test_posts_filters_and_config(self) -> None:
        since = self.client.seq()
        for subject in ("one", "two"):
            http_request("POST", f"{self.base}/emails", data=json.dumps({
                "from": "a@corp.test", "to": "x@ext.test", "subject": subject, "text": "t"}).encode(),
                headers={"Authorization": "Bearer re_k", "Content-Type": "application/json",
                         "Idempotency-Key": f"k-{subject}"})
        self.assertEqual([p["body"]["subject"] for p in self.client.posts(since)], ["one", "two"])
        self.assertEqual(len(self.client.posts(since, subject="two")), 1)
        self.assertEqual(len(self.client.posts(since, idempotency_key="k-one")), 1)
        self.assertEqual([e["subject"] for e in self.client.emails(since)], ["one", "two"])
        self.client.config(meta_delay=3)
        self.client.faults([{"match": "POST /emails", "status": 500}])
        self.client.soft_reset()
        self.assertEqual(self.client.config()["meta_delay"], 0.0)
        self.assertEqual(self.client.faults(), [])
        self.assertEqual(len(self.client.emails(since)), 2, "soft reset keeps data")


class RawRequestTests(unittest.TestCase):
    def test_early_413(self) -> None:
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]

        def serve() -> None:
            conn, _ = srv.accept()
            data = b""
            while b"\r\n\r\n" not in data:
                data += conn.recv(4096)
            body = b'{"error":{"code":"payload_too_large"}}'
            conn.sendall(b"HTTP/1.1 413 Payload Too Large\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s"
                         % (len(body), body))
            conn.close()

        t = threading.Thread(target=serve)
        t.start()
        status, headers, body = raw_request("127.0.0.1", port, "POST", "/api/attachments",
                                            {"Content-Length": str(26 << 20)})
        t.join()
        srv.close()
        self.assertEqual(status, 413)
        self.assertIn(b"payload_too_large", body)
        self.assertEqual(headers["connection"], "close")


class SigningTests(unittest.TestCase):
    def test_format_matches_backend(self) -> None:
        secret = "e2e.secret"
        signer = signing.SignedUrls(secret.encode(), "http://api.test/")
        key = hmac.new(b"e2e.secret", b"azmail/signed-url/v1", hashlib.sha256).digest()
        mac = hmac.new(key, b"file|12|3|1700000000000|a", hashlib.sha256).digest()
        expected_sig = base64.urlsafe_b64encode(mac).decode().rstrip("=")
        self.assertEqual(signer.file_url(12, 3, "a", 1700000000000),
                         f"http://api.test/api/files/12?d=a&u=3&exp=1700000000000&sig={expected_sig}")
        self.assertTrue(signer.raw_url(5, 3, 9).startswith("http://api.test/api/files/raw/5?u=3&exp=9&sig="))
        self.assertNotIn("=", signer.sign_raw(5, 3, 9))

    def test_detect_secret_interpretation(self) -> None:
        hex_secret = "00ff" * 16
        real = signing.SignedUrls(bytes.fromhex(hex_secret), "http://a")  # loader hex-decoded it
        url = real.file_url(1, 2, "i", 3)
        found = signing.detect(hex_secret, "http://a", url)
        self.assertIsNotNone(found)
        self.assertTrue(found.reproduces(real.raw_url(4, 5, 6)))
        self.assertIsNone(signing.detect("other", "http://a", url))
        self.assertFalse(real.reproduces("http://a/api/files/1?d=i&u=2&exp=3&sig=nope"))


class WaitTests(unittest.TestCase):
    def test_eventually(self) -> None:
        calls = {"n": 0}

        def flaky() -> int:
            calls["n"] += 1
            assert calls["n"] >= 3, "not yet"
            return calls["n"]

        self.assertEqual(eventually(flaky, 2, 0.01), 3)
        with self.assertRaises(AssertionError) as ctx:
            eventually(lambda: (_ for _ in ()).throw(AssertionError("never ok")), 0.2, 0.05, "thing")
        self.assertIn("thing", str(ctx.exception))
        self.assertIn("never ok", str(ctx.exception))
        with self.assertRaises(ZeroDivisionError):  # other exceptions are not swallowed
            eventually(lambda: 1 / 0, 1)

    def test_wait_for_and_never(self) -> None:
        t0 = time.monotonic()
        self.assertEqual(wait_for(lambda: time.monotonic() - t0 > 0.1 and "ok", 2, 0.01), "ok")
        never(lambda: False, 0.1, 0.02)
        with self.assertRaises(AssertionError):
            never(lambda: True, 0.5, 0.02, "flag")


class ProcTests(unittest.TestCase):
    def test_start_stop_kill_and_logs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "p.log"
            script = "import time,sys; print('hello', flush=True); time.sleep(30)"
            p = Proc("sleeper", [sys.executable, "-c", script], os.environ, log, redact=["secret-x"]).start()
            wait_for(lambda: "hello" in tail_file(log), 5, 0.05)
            self.assertTrue(p.alive())
            p.stop(timeout=5)
            self.assertFalse(p.alive())
            p2 = Proc("victim", [sys.executable, "-c", script, "secret-x"], os.environ, log,
                      redact=["secret-x"]).start()
            p2.kill()
            self.assertEqual(p2.returncode, -9)
            self.assertNotIn("secret-x", log.read_text())
            self.assertIn("===== victim exited (-9)", log.read_text())

    def test_run_cli_and_ports(self) -> None:
        res = run_cli([sys.executable, "-c", "import sys; print(sys.stdin.read().upper()); sys.exit(3)"],
                      os.environ, stdin="pw\n")
        self.assertEqual((res.returncode, res.stdout.strip()), (3, "PW"))
        res = run_cli([sys.executable, "-c", "import time; time.sleep(5)"], os.environ, timeout=0.3)
        self.assertEqual(res.returncode, -9)
        res = run_cli(["/nonexistent/azmail"], os.environ)
        self.assertEqual(res.returncode, -1)
        port = free_port()
        self.assertTrue(1024 < port < 65536)


class RunnerCliTests(unittest.TestCase):
    def _run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(HERE / "run.py"), *args], capture_output=True,
                              text=True, timeout=60)

    def test_list(self) -> None:
        res = self._run("--list")
        self.assertEqual(res.returncode, 0, res.stderr)
        names = [l.split()[0] for l in res.stdout.splitlines() if l.startswith("s")]
        self.assertEqual(names, sorted(names))
        self.assertEqual([n[:3] for n in names], [f"s{i:02d}" for i in range(1, 37)])
        self.assertIn("36 scenario(s)", res.stdout)
        res = self._run("--list", "-k", "s03,undo")
        self.assertTrue(all("s03" in l or "undo" in l.lower() for l in res.stdout.splitlines()[:-1]))

    def test_missing_binary_and_bad_filter(self) -> None:
        res = self._run("--azmail-bin", "/nonexistent/azmail", "-k", "s01")
        self.assertEqual(res.returncode, 2)
        self.assertIn("azmail binary not found", res.stderr)
        res = self._run("-k", "no-such-scenario-xyz")
        self.assertEqual(res.returncode, 2)

    def test_scenarios_have_contract(self) -> None:
        sys.path.insert(0, str(HERE))
        import importlib

        for path in sorted((HERE / "scenarios").glob("s[0-9][0-9]_*.py")):
            mod = importlib.import_module(f"scenarios.{path.stem}")
            self.assertTrue(callable(getattr(mod, "run", None)), path.name)
            self.assertTrue(getattr(mod, "TITLE", "") or mod.__doc__, path.name)
            self.assertGreater(int(getattr(mod, "TIMEOUT", 120)), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
