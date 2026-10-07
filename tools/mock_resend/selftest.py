"""Self-test of the mock: Svix + AWS SigV4 published vectors and the core Resend / R2 flows.

    python3 -m tools.mock_resend.selftest [-v]

Hermetic: everything runs in-process on 127.0.0.1 ephemeral ports (no real network).
"""

from __future__ import annotations

import base64
import hashlib
import http.client
import json
import socket
import sys
import threading
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from . import eml, sigv4, svix
from .server import DEFAULT_S3_ACCESS_KEY, DEFAULT_S3_SECRET_KEY, MockResend, parse_iso, pg_ts

API_KEY = "re_selftest_key"
SECRET = svix.new_secret()
UA = "azmail-selftest/1.0"

# AWS SigV4 S3 examples (DESIGN Addendum A.2)
AK = "AKIAIOSFODNN7EXAMPLE"
SK = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
AMZ = "20130524T000000Z"
AWS_HOST = "examplebucket.s3.amazonaws.com"
AWS_NOW = 1369353600.0  # 2013-05-24T00:00:00Z


def wait_until(pred, timeout: float = 5.0, interval: float = 0.02):
    deadline = time.time() + timeout
    while True:
        value = pred()
        if value:
            return value
        if time.time() > deadline:
            raise AssertionError("condition not met within %.1fs" % timeout)
        time.sleep(interval)


class SvixVectorTests(unittest.TestCase):
    def test_svix_docs_vector(self) -> None:
        sig = svix.sign("whsec_plJ3nmyCDGBKInavdOK15jsl", "msg_loFOjxBNrRLzqYUf", "1731705121",
                        '{"event_type":"ping","data":{"success":true}}')
        self.assertEqual(sig, "v1,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0=")

    def test_svix_library_vector(self) -> None:
        sig = svix.sign("whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw", "msg_p5jXN8AQM9LWM0D4loKWxJek",
                        "1614265330", '{"test": 2432232314}')
        self.assertEqual(sig, "v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE=")

    def test_verify(self) -> None:
        body = '{"event_type":"ping","data":{"success":true}}'
        sec, mid, ts = "whsec_plJ3nmyCDGBKInavdOK15jsl", "msg_loFOjxBNrRLzqYUf", "1731705121"
        good = "v1,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0="
        self.assertTrue(svix.verify(sec, mid, ts, good, body, now=1731705121))
        self.assertTrue(svix.verify(sec, mid, ts, "v1,bogus " + good, body, now=1731705121 + 299))
        self.assertFalse(svix.verify(sec, mid, ts, good, body, now=1731705121 + 301))
        self.assertFalse(svix.verify(sec, mid, ts, good, body + " ", now=1731705121))
        self.assertFalse(svix.verify(sec, mid, ts, "v2," + good[3:], body, now=1731705121))
        self.assertFalse(svix.verify(sec, "", ts, good, body, now=1731705121))
        with self.assertRaises(ValueError):
            svix.sign("nope", mid, ts, body)


class SigV4VectorTests(unittest.TestCase):
    def test_signing_key(self) -> None:
        self.assertEqual(sigv4.signing_key(SK, "20130524", "us-east-1", "s3").hex(),
                         "dbb893acc010964918f1fd433add87c70e8b0db6be30c1fbeafefa5ec6ba8378")

    def test_get_object(self) -> None:
        r = sigv4.sign_request(AK, SK, "GET", "/test.txt", [],
                               [("host", AWS_HOST), ("range", "bytes=0-9"),
                                ("x-amz-content-sha256", sigv4.EMPTY_SHA256), ("x-amz-date", AMZ)],
                               sigv4.EMPTY_SHA256, AMZ, "us-east-1")
        self.assertEqual(sigv4.sha256_hex(r.canonical_request),
                         "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972")
        self.assertEqual(r.signature, "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41")
        self.assertEqual(r.signed_headers, "host;range;x-amz-content-sha256;x-amz-date")

    def test_put_object(self) -> None:
        body_hash = sigv4.sha256_hex(b"Welcome to Amazon S3.")
        self.assertEqual(body_hash, "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072")
        r = sigv4.sign_request(AK, SK, "PUT", "/test$file.text", [],
                               [("date", "Fri, 24 May 2013 00:00:00 GMT"), ("host", AWS_HOST),
                                ("x-amz-content-sha256", body_hash), ("x-amz-date", AMZ),
                                ("x-amz-storage-class", "REDUCED_REDUNDANCY")],
                               body_hash, AMZ, "us-east-1")
        self.assertIn("\n/test%24file.text\n", r.canonical_request)
        self.assertEqual(sigv4.sha256_hex(r.canonical_request),
                         "9e0e90d9c76de8fa5b200d8c849cd5b8dc7a3be3951ddb7f6a76b4158342019d")
        self.assertEqual(r.signature, "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd")

    def test_bucket_lifecycle_and_list(self) -> None:
        hdrs = [("host", AWS_HOST), ("x-amz-content-sha256", sigv4.EMPTY_SHA256), ("x-amz-date", AMZ)]
        r = sigv4.sign_request(AK, SK, "GET", "/", [("lifecycle", "")], hdrs, sigv4.EMPTY_SHA256, AMZ,
                               "us-east-1")
        self.assertIn("\nlifecycle=\n", r.canonical_request)
        self.assertEqual(r.signature, "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543")
        r = sigv4.sign_request(AK, SK, "GET", "/", [("prefix", "J"), ("max-keys", "2")], hdrs,
                               sigv4.EMPTY_SHA256, AMZ, "us-east-1")
        self.assertIn("\nmax-keys=2&prefix=J\n", r.canonical_request)
        self.assertEqual(r.signature, "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7")

    def test_presigned_get(self) -> None:
        p = sigv4.presign(AK, SK, host=AWS_HOST, raw_path="/test.txt", amz_date_value=AMZ,
                          expires=86400, region="us-east-1")
        self.assertEqual(sigv4.sha256_hex(p.canonical_request),
                         "3bfa292879f6447bbcda7001decf97f4a54dc650c8942174ae0a9121cf58ad04")
        self.assertEqual(p.signature, "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404")
        self.assertTrue(p.url.startswith("https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm="))
        self.assertIn("X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request", p.url)

    def test_encoding_rules(self) -> None:
        self.assertEqual(sigv4.uri_encode("a b+c/d~é"), "a%20b%2Bc%2Fd~%C3%A9")
        self.assertEqual(sigv4.canonical_uri("/b/x y/z"), "/b/x%20y/z")
        self.assertEqual(sigv4.canonical_uri(""), "/")
        self.assertEqual(sigv4.canonical_query([("b", "2"), ("a", "x/y"), ("a", "1")]), "a=1&a=x%2Fy&b=2")
        block, signed = sigv4.canonical_headers([("X-B", "  a   b "), ("host", "h"), ("x-b", "c")])
        self.assertEqual(block, "host:h\nx-b:a b,c\n")
        self.assertEqual(signed, "host;x-b")


class SigV4VerifyTests(unittest.TestCase):
    """The server-side verifier accepts the AWS examples and rejects tampering."""

    keys = {AK: SK}
    get_headers = [("Host", AWS_HOST), ("Range", "bytes=0-9"),
                   ("x-amz-content-sha256", sigv4.EMPTY_SHA256), ("x-amz-date", AMZ)]

    def _auth(self, sig: str = "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41") -> str:
        return (f"AWS4-HMAC-SHA256 Credential={AK}/20130524/us-east-1/s3/aws4_request,"
                f"SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,Signature={sig}")

    def verify(self, headers, **kw):
        return sigv4.verify("GET", "/test.txt", kw.pop("query", []), headers, self.keys,
                            now=kw.pop("now", AWS_NOW), regions=("us-east-1",), **kw)

    def test_header_auth_ok(self) -> None:
        res = self.verify(self.get_headers + [("Authorization", self._auth())])
        self.assertEqual(res.mode, "header")

    def test_header_auth_failures(self) -> None:
        cases = [
            (self.get_headers + [("Authorization", self._auth("0" * 64))], "SignatureDoesNotMatch"),
            ([("Host", AWS_HOST), ("Range", "bytes=0-10"), ("x-amz-content-sha256", sigv4.EMPTY_SHA256),
              ("x-amz-date", AMZ), ("Authorization", self._auth())], "SignatureDoesNotMatch"),
            (self.get_headers, "AccessDenied"),
            (self.get_headers + [("x-amz-meta-x", "1"), ("Authorization", self._auth())], "AccessDenied"),
            ([h for h in self.get_headers if h[0] != "x-amz-content-sha256"]
             + [("Authorization", self._auth())], "InvalidRequest"),
            (self.get_headers + [("Authorization", self._auth().replace(AK, "AKIDUNKNOWN"))],
             "InvalidAccessKeyId"),
            (self.get_headers + [("Authorization", self._auth().replace("us-east-1", "auto"))],
             "AuthorizationHeaderMalformed"),
        ]
        for headers, code in cases:
            with self.subTest(code=code):
                with self.assertRaises(sigv4.AuthFailure) as ctx:
                    self.verify(headers)
                self.assertEqual(ctx.exception.code, code)
        with self.assertRaises(sigv4.AuthFailure) as ctx:
            self.verify(self.get_headers + [("Authorization", self._auth())], now=AWS_NOW + 3600)
        self.assertEqual(ctx.exception.code, "RequestTimeTooSkewed")

    def test_payload_mismatch(self) -> None:
        body = b"Welcome to Amazon S3."
        claimed = sigv4.sha256_hex(b"something else")
        r = sigv4.sign_request(AK, SK, "PUT", "/k", [], [("host", AWS_HOST), ("x-amz-content-sha256", claimed),
                                                         ("x-amz-date", AMZ)], claimed, AMZ, "us-east-1")
        hdrs = [("host", AWS_HOST), ("x-amz-content-sha256", claimed), ("x-amz-date", AMZ),
                ("Authorization", r.authorization)]
        with self.assertRaises(sigv4.AuthFailure) as ctx:
            sigv4.verify("PUT", "/k", [], hdrs, self.keys, now=AWS_NOW, regions=("us-east-1",),
                         body_sha256=sigv4.sha256_hex(body))
        self.assertEqual(ctx.exception.code, "XAmzContentSHA256Mismatch")

    def test_presigned(self) -> None:
        p = sigv4.presign(AK, SK, host=AWS_HOST, raw_path="/test.txt", amz_date_value=AMZ,
                          expires=86400, region="us-east-1")
        query = [(urllib.parse.unquote_plus(k), urllib.parse.unquote_plus(v)) for k, _, v in
                 (part.partition("=") for part in urllib.parse.urlsplit(p.url).query.split("&"))]
        res = self.verify([("Host", AWS_HOST)], query=query, now=AWS_NOW + 100)
        self.assertEqual(res.mode, "presigned")
        with self.assertRaises(sigv4.AuthFailure) as ctx:
            self.verify([("Host", AWS_HOST)], query=query, now=AWS_NOW + 86401)
        self.assertEqual((ctx.exception.code, ctx.exception.message), ("AccessDenied", "Request has expired"))
        tampered = [(k, v[:-1] + ("0" if v[-1] != "0" else "1")) if k == "X-Amz-Signature" else (k, v)
                    for k, v in query]
        with self.assertRaises(sigv4.AuthFailure) as ctx:
            self.verify([("Host", AWS_HOST)], query=tampered, now=AWS_NOW + 100)
        self.assertEqual(ctx.exception.code, "SignatureDoesNotMatch")
        with self.assertRaises(sigv4.AuthFailure) as ctx:
            self.verify([("Host", "other.host")], query=query, now=AWS_NOW + 100)
        self.assertEqual(ctx.exception.code, "SignatureDoesNotMatch")
        with self.assertRaises(ValueError):
            sigv4.presign(AK, SK, host=AWS_HOST, raw_path="/x", amz_date_value=AMZ, expires=604801)


class EmlTests(unittest.TestCase):
    def test_build_and_headers(self) -> None:
        raw = eml.build_eml(
            from_='"张三 (运营)" <zhang@ext.test>', to=["爱丽丝 <alice@corp.test>"], cc=["c@corp.test"],
            subject="季度报告 Q3", html='<p><img src="cid:img1"></p>', text="hi",
            message_id="abc@ext.test", date=1700000000,
            headers=[("In-Reply-To", "<p@x>"), ("References", "<o@x> <p@x>"), ("X-AzMail-Ref", "u1")],
            attachments=[eml.Part("季度报告 2026.pdf", "application/pdf", b"%PDF"),
                         eml.Part("logo.png", "image/png", b"PNG", content_id="img1", inline=True)],
            auth_results=eml.auth_results_header("pass", "pass", "fail", "ext.test"))
        self.assertIn(b"\r\n", raw)
        h = eml.headers_map(raw)
        self.assertEqual(h["message-id"], "<abc@ext.test>")
        self.assertEqual(h["x-azmail-ref"], "u1")
        self.assertEqual(h["references"], "<o@x> <p@x>")
        self.assertIn("dmarc=fail", h["authentication-results"])
        self.assertTrue(h["subject"].startswith("=?utf-8?"))
        self.assertEqual(eml.decode_words(h["subject"]), "季度报告 Q3")
        self.assertIn("张三 (运营)", eml.decode_words(h["from"]))
        self.assertIn(b"filename*=utf-8''%E5%AD%A3%E5%BA%A6", raw)
        self.assertIn(b"Content-ID: <img1>", raw)
        self.assertNotIn(b"Bcc", raw)

    def test_subject_charset(self) -> None:
        raw = eml.build_eml(from_="a@x.test", to=["b@y.test"], subject="周报", text="x",
                            message_id="<m@x>", date=0, subject_charset="gb18030")
        value = eml.headers_map(raw)["subject"]
        self.assertTrue(value.lower().startswith("=?gb18030?"))
        self.assertEqual(eml.decode_words(value), "周报")

    def test_time_formats(self) -> None:
        self.assertEqual(pg_ts(0), "1970-01-01 00:00:00.000000+00")
        self.assertAlmostEqual(parse_iso("2026-10-07T12:00:00.123Z"), 1791374400.123, places=3)
        self.assertEqual(parse_iso("2026-10-07T20:00:00+08:00"), parse_iso("2026-10-07T12:00:00Z"))
        self.assertEqual(parse_iso("2026-10-07 12:00:00.123456789+00"), parse_iso("2026-10-07T12:00:00.123456Z"))
        for bad in ("in 1 min", "tomorrow at 9am", "2026-10-07T12:00:00"):
            with self.assertRaises(ValueError):
                parse_iso(bad)


# ---------------------------------------------------------------------------------------------
# in-process server tests
# ---------------------------------------------------------------------------------------------

class _Receiver(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    events: list[dict[str, Any]] = []
    status = 200
    lock = threading.Lock()

    def log_message(self, *args: Any) -> None:
        pass

    def do_POST(self) -> None:
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        ok = svix.verify(SECRET, self.headers.get("svix-id", ""), self.headers.get("svix-timestamp", ""),
                         self.headers.get("svix-signature", ""), body)
        with _Receiver.lock:
            status = _Receiver.status
            _Receiver.events.append({"svix_id": self.headers.get("svix-id"), "verified": ok,
                                     "payload": json.loads(body), "status": status})
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()


class ServerTests(unittest.TestCase):
    mock: MockResend
    port: int
    s3_port: int

    @classmethod
    def setUpClass(cls) -> None:
        cls.receiver = ThreadingHTTPServer(("127.0.0.1", 0), _Receiver)
        cls.receiver.daemon_threads = True
        threading.Thread(target=cls.receiver.serve_forever, daemon=True).start()
        hook = f"http://127.0.0.1:{cls.receiver.server_address[1]}/api/webhooks/resend"
        cls.mock = MockResend(api_key=API_KEY, webhook_url=hook, webhook_secret=SECRET,
                              local_domains=["corp.test"], time_scale=0.05, rate_limit=0,
                              s3_buckets=["azmail-test"], s3_write_interval=5.0)
        cls.port, cls.s3_port = cls.mock.serve("127.0.0.1", 0, 0)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.mock.shutdown()
        cls.receiver.shutdown()
        cls.receiver.server_close()

    def setUp(self) -> None:
        self.ctl("POST", "/_mock/reset", {"s3": True})
        with _Receiver.lock:
            _Receiver.events.clear()
            _Receiver.status = 200

    # -- helpers --------------------------------------------------------------------------------
    def http(self, method: str, path: str, body: Any = None, headers: dict[str, str] | None = None,
             port: int | None = None) -> tuple[int, dict[str, str], bytes]:
        conn = http.client.HTTPConnection("127.0.0.1", port or self.port, timeout=15)
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        hdrs = dict(headers or {})
        if data is not None:
            hdrs.setdefault("Content-Type", "application/json")
        conn.request(method, path, body=data, headers=hdrs)
        resp = conn.getresponse()
        out = resp.read()
        result = resp.status, {k.lower(): v for k, v in resp.getheaders()}, out
        conn.close()
        return result

    def api(self, method: str, path: str, body: Any = None, *, key: str | None = API_KEY,
            ua: str | None = UA, extra: dict[str, str] | None = None) -> tuple[int, dict[str, str], Any]:
        headers = {}
        if key is not None:
            headers["Authorization"] = f"Bearer {key}"
        if ua is not None:
            headers["User-Agent"] = ua
        else:
            headers["User-Agent"] = ""
        headers.update(extra or {})
        status, hdrs, raw = self.http(method, path, body, headers)
        try:
            parsed = json.loads(raw) if raw else None
        except ValueError:
            parsed = raw
        return status, hdrs, parsed

    def ctl(self, method: str, path: str, body: Any = None) -> Any:
        status, _, raw = self.http(method, path, body)
        self.assertEqual(status, 200, raw)
        return json.loads(raw) if raw.startswith(b"{") or raw.startswith(b"[") else raw

    def send(self, **overrides: Any) -> tuple[int, Any]:
        body = {"from": "Alice <alice@corp.test>", "to": ["bob@corp.test"], "subject": "Hello",
                "html": "<p>hi</p>", "text": "hi"}
        body.update(overrides)
        status, _, out = self.api("POST", "/emails", body)
        return status, out

    def events(self, email_id: str | None = None) -> list[dict[str, Any]]:
        with _Receiver.lock:
            evs = list(_Receiver.events)
        if email_id is None:
            return evs
        return [e for e in evs if e["payload"]["data"].get("email_id") == email_id]

    def wait_idle(self) -> None:
        wait_until(lambda: self.mock.idle(), timeout=10)

    # -- edge behaviour ---------------------------------------------------------------------------
    def test_user_agent_auth_and_rate_limit(self) -> None:
        status, hdrs, body = self.api("GET", "/domains", ua=None)
        self.assertEqual((status, body), (403, b"error code: 1010"))
        self.assertTrue(hdrs["content-type"].startswith("text/plain"))
        status, _, body = self.api("GET", "/domains", key=None)
        self.assertEqual((status, body["name"]), (401, "missing_api_key"))
        status, _, body = self.api("GET", "/domains", key="re_wrong")
        self.assertEqual((status, body["name"], body["statusCode"]), (403, "invalid_api_key", 403))
        self.ctl("POST", "/_mock/config", {"rate_limit": 3})
        time.sleep(1.0 - (time.time() % 1.0) + 0.01)  # start of a fresh window
        results = [self.api("GET", "/domains") for _ in range(10)]
        self.ctl("POST", "/_mock/config", {"rate_limit": 0})
        self.assertEqual(results[0][0], 200)
        limited = [r for r in results if r[0] == 429]
        self.assertTrue(limited, "10 requests at 3 rps must hit the limit")
        self.assertTrue(all(r[0] in (200, 429) for r in results))
        self.assertEqual(limited[0][2]["name"], "rate_limit_exceeded")
        self.assertEqual(limited[0][1]["retry-after"], "1")
        self.assertEqual(limited[0][1]["ratelimit-remaining"], "0")
        self.assertEqual(results[0][1]["ratelimit-limit"], "3")
        domains = results[0][2]["data"]
        self.assertEqual([d["name"] for d in domains], ["corp.test"])
        status, _, dom = self.api("GET", f"/domains/{domains[0]['id']}")
        self.assertEqual(status, 200)
        self.assertIn("inbound-smtp.us-east-1.amazonaws.com", [r["value"] for r in dom["records"]])

    def test_send_validation(self) -> None:
        big_to = [f"u{i}@ext.test" for i in range(51)]
        cases = [
            ({"to": None}, 422, "missing_required_field"),
            ({"to": big_to}, 422, "validation_error"),
            ({"to": ["not-an-address"]}, 422, "validation_error"),
            ({"from": "x@unverified.test"}, 403, "validation_error"),
            ({"subject": None}, 422, "missing_required_field"),
            ({"html": None, "text": None}, 422, "missing_required_field"),
            ({"attachments": [{"filename": "a.txt", "content": "@@not base64@@"}]}, 422, "invalid_attachment"),
            ({"attachments": [{"filename": "a.png", "content": "aGk=", "content_id": "x" * 128}]}, 422, "validation_error"),
            ({"attachments": [{"filename": "a.txt", "path": "https://x.test/a"}]}, 422, "invalid_attachment"),
            ({"tags": [{"name": "bad tag", "value": "x"}]}, 422, "validation_error"),
            ({"scheduled_at": "in 1 min"}, 422, "validation_error"),
            ({"scheduled_at": "2020-01-01T00:00:00Z"}, 422, "validation_error"),
            ({"scheduled_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + 31 * 86400))}, 422, "validation_error"),
            ({"headers": {"X-A": 1}}, 422, "validation_error"),
        ]
        for override, status, name in cases:
            with self.subTest(override=str(override)[:60]):
                got_status, out = self.send(**override)
                self.assertEqual((got_status, out["name"]), (status, name), out)
        ok_status, out = self.send(to=[f"u{i}@ext.test" for i in range(50)], subject="",
                                   attachments=[{"filename": "a.png", "content": "aGk=", "content_id": "x" * 127}])
        self.assertEqual(ok_status, 200, out)
        self.ctl("POST", "/_mock/config", {"reject_scheduled_attachments": True})
        at = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(time.time() + 3600))
        st, out = self.send(scheduled_at=at, attachments=[{"filename": "a.txt", "content": "aGk="}])
        self.assertEqual(st, 422)
        self.assertIn("scheduled", out["message"])

    def test_idempotency(self) -> None:
        body = {"from": "alice@corp.test", "to": "x@ext.test", "subject": "S", "text": "t"}
        h = {"Idempotency-Key": "key-1"}
        s1, _, r1 = self.api("POST", "/emails", body, extra=h)
        s2, _, r2 = self.api("POST", "/emails", body, extra=h)
        self.assertEqual((s1, s2), (200, 200))
        self.assertEqual(r1, r2)
        s3, _, r3 = self.api("POST", "/emails", dict(body, subject="other"), extra=h)
        self.assertEqual((s3, r3["name"]), (409, "invalid_idempotent_request"))
        sent = self.ctl("GET", "/_mock/sent?path=/emails")
        self.assertEqual(len(sent["emails"]), 1)
        self.assertEqual([r["replayed"] for r in sent["requests"]], [False, True, False])
        self.assertEqual(sent["requests"][0]["user_agent"], UA)
        self.assertEqual(sent["requests"][0]["idempotency_key"], "key-1")
        s4, _, r4 = self.api("POST", "/emails", body, extra={"Idempotency-Key": "k" * 257})
        self.assertEqual((s4, r4["name"]), (400, "invalid_idempotency_key"))
        # in flight: the first request is held (delay fault) while the second arrives
        self.ctl("POST", "/_mock/faults", [{"match": "POST /emails", "delay": 0.6, "count": 1}])
        result: dict[str, Any] = {}

        def first() -> None:
            result["first"] = self.api("POST", "/emails", body, extra={"Idempotency-Key": "key-2"})

        t = threading.Thread(target=first)
        t.start()
        wait_until(lambda: any(r["idempotency_key"] == "key-2" for r in
                               self.ctl("GET", "/_mock/sent?path=/emails")["requests"]))
        time.sleep(0.05)  # the first request is now holding the key (0.6 s delay fault)
        s5, _, r5 = self.api("POST", "/emails", body, extra={"Idempotency-Key": "key-2"})
        t.join()
        self.assertEqual((s5, r5["name"]), (409, "concurrent_idempotent_requests"))
        self.assertEqual(result["first"][0], 200)
        # validation failures are not cached: the key stays usable
        s6, _, _ = self.api("POST", "/emails", dict(body, to="bad"), extra={"Idempotency-Key": "key-3"})
        s7, _, _ = self.api("POST", "/emails", body, extra={"Idempotency-Key": "key-3"})
        self.assertEqual((s6, s7), (422, 200))

    def test_faults(self) -> None:
        self.ctl("POST", "/_mock/faults", [
            {"match": "POST /emails", "status": 429, "name": "rate_limit_exceeded", "retry_after": 2},
            {"match": "POST /emails", "status": 500, "name": "internal_server_error", "count": 2},
            {"match": "GET /emails/*", "status": 502, "body": "<html>bad gateway</html>"},
        ])
        statuses = []
        for _ in range(4):
            s, hdrs, out = self.api("POST", "/emails", {"from": "alice@corp.test", "to": "x@ext.test",
                                                        "subject": "S", "text": "t"})
            statuses.append(s)
            if s == 429:
                self.assertEqual(hdrs["retry-after"], "2")
        self.assertEqual(statuses, [429, 500, 500, 200])
        s, hdrs, out = self.api("GET", "/emails/whatever")
        self.assertEqual((s, out), (502, b"<html>bad gateway</html>"))
        # timeout: processed, response held
        self.ctl("POST", "/_mock/faults", [{"match": "POST /emails", "timeout": 0.5}])
        t0 = time.time()
        s, out = self.send(to="y@ext.test", subject="timeout")
        self.assertGreaterEqual(time.time() - t0, 0.5)
        self.assertEqual(s, 200)
        faults = self.ctl("GET", "/_mock/faults")["faults"]
        self.assertEqual(faults[0]["count"], 0)
        status, _, raw = self.http("POST", "/_mock/faults", [{"match": "POST /emails"}])
        self.assertEqual(status, 400)

    # -- delivery -----------------------------------------------------------------------------------
    def test_loopback_delivery_and_receiving(self) -> None:
        png = b"\x89PNG\r\n\x1a\nfake"
        pdf = b"%PDF-1.4 hello"
        s, out = self.send(
            to=["Bob <bob@corp.test>", "partner@ext.test"], cc=["carol@corp.test"], bcc=["dave@corp.test"],
            subject="季度报告 · Q3", html='<p>见附件 <img src="cid:logo-1"></p>',
            headers={"X-AzMail-Ref": "uuid-1", "In-Reply-To": "<parent@corp.test>",
                     "References": "<root@corp.test> <parent@corp.test>"},
            tags=[{"name": "azmail_outbound", "value": "uuid-1"}],
            attachments=[{"filename": "logo.png", "content": base64.b64encode(png).decode(),
                          "content_type": "image/png", "content_id": "logo-1"},
                         {"filename": "季度报告 2026.pdf", "content": base64.b64encode(pdf).decode()}])
        self.assertEqual(s, 200, out)
        eid = out["id"]
        self.wait_idle()
        evs = self.events()
        self.assertTrue(all(e["verified"] for e in evs))
        types = [e["payload"]["type"] for e in evs]
        self.assertEqual([t for t in types if t != "email.received"], ["email.sent", "email.delivered"])
        self.assertEqual(types.count("email.received"), 1)
        sent_ev = next(e for e in evs if e["payload"]["type"] == "email.sent")["payload"]
        self.assertEqual(sent_ev["data"]["tags"], {"azmail_outbound": "uuid-1"})
        self.assertEqual(sent_ev["data"]["email_id"], eid)
        full = self.ctl("GET", f"/_mock/emails/{eid}")
        self.assertEqual(sent_ev["data"]["message_id"], full["message_id"])
        status, _, got = self.api("GET", f"/emails/{eid}")
        self.assertEqual(status, 200)
        self.assertEqual(got["last_event"], "delivered")
        self.assertEqual(got["message_id"], full["message_id"])
        self.assertRegex(got["created_at"], r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{6}\+00$")
        self.assertEqual(got["tags"], [{"name": "azmail_outbound", "value": "uuid-1"}])

        rec_ev = next(e for e in evs if e["payload"]["type"] == "email.received")["payload"]["data"]
        self.assertEqual(sorted(rec_ev["received_for"]), ["bob@corp.test", "carol@corp.test", "dave@corp.test"])
        rid = rec_ev["email_id"]
        status, _, lst = self.api("GET", "/emails/receiving?limit=10")
        self.assertEqual([r["id"] for r in lst["data"]], [rid])
        status, _, rec = self.api("GET", f"/emails/receiving/{rid}?html_format=cid")
        self.assertEqual(status, 200)
        self.assertIn("cid:logo-1", rec["html"])
        self.assertEqual(rec["html_format"], "cid")
        self.assertEqual(rec["message_id"], full["message_id"])
        self.assertEqual(rec["headers"]["x-azmail-ref"], "uuid-1")
        self.assertEqual(rec["headers"]["in-reply-to"], "<parent@corp.test>")
        self.assertEqual(rec["authentication"], {"spf": "pass", "dkim": "pass", "dmarc": "pass"})
        self.assertEqual(rec["subject"], "季度报告 · Q3")
        self.assertEqual({a["filename"]: a["content_disposition"] for a in rec["attachments"]},
                         {"logo.png": "inline", "季度报告 2026.pdf": "attachment"})
        status, _, rec_data = self.api("GET", f"/emails/receiving/{rid}")
        self.assertIn("data:image/png;base64,", rec_data["html"])
        # attachments: download_url -> 302 /_blob -> bytes; Authorization header rejected
        status, _, atts = self.api("GET", f"/emails/receiving/{rid}/attachments")
        by_name = {a["filename"]: a for a in atts["data"]}
        url = urllib.parse.urlsplit(by_name["季度报告 2026.pdf"]["download_url"])
        self.assertTrue(url.path.startswith("/_dl/"))
        status, hdrs, _ = self.http("GET", f"{url.path}?{url.query}", headers={"Authorization": f"Bearer {API_KEY}"})
        self.assertEqual(status, 400)
        self.assertEqual(len(self.ctl("GET", "/_mock/violations")["violations"]), 1)
        status, hdrs, _ = self.http("GET", f"{url.path}?{url.query}")
        self.assertEqual(status, 302)
        loc = urllib.parse.urlsplit(hdrs["location"])
        status, hdrs, data = self.http("GET", f"{loc.path}?{loc.query}")
        self.assertEqual((status, data), (200, pdf))
        self.assertIn("filename*=UTF-8''%E5%AD%A3", hdrs["content-disposition"])
        status, _, _ = self.http("GET", f"{url.path}?exp=1")
        self.assertEqual(status, 403)
        # raw
        raw_url = urllib.parse.urlsplit(rec["raw"]["download_url"])
        status, hdrs, _ = self.http("GET", f"{raw_url.path}?{raw_url.query}")
        loc = urllib.parse.urlsplit(hdrs["location"])
        status, hdrs, raw = self.http("GET", f"{loc.path}?{loc.query}")
        self.assertEqual(hdrs["content-type"], "message/rfc822")
        self.assertEqual(raw, self.ctl("GET", f"/_mock/received/{rid}/raw"))
        self.assertIn(b"X-AzMail-Ref: uuid-1", raw)
        self.assertNotIn(b"dave@corp.test", raw)  # BCC never appears in headers
        # unknown ids
        self.assertEqual(self.api("GET", "/emails/receiving/nope")[0], 404)
        self.assertEqual(self.api("GET", "/emails/nope")[0], 404)

    def test_external_outcomes(self) -> None:
        expected = {
            "bounce@ext.test": ["email.sent", "email.bounced"],
            "fail@ext.test": ["email.failed"],
            "delay@ext.test": ["email.sent", "email.delivery_delayed", "email.delivered"],
            "complain@ext.test": ["email.sent", "email.delivered", "email.complained"],
            "suppress@ext.test": ["email.sent", "email.suppressed"],
            "ok@ext.test": ["email.sent", "email.delivered"],
        }
        ids = {}
        for rcpt in expected:
            s, out = self.send(to=rcpt)
            self.assertEqual(s, 200)
            ids[rcpt] = out["id"]
        self.wait_idle()
        for rcpt, types in expected.items():
            with self.subTest(rcpt=rcpt):
                evs = self.events(ids[rcpt])
                self.assertEqual([e["payload"]["type"] for e in evs], types)
                self.assertEqual(self.api("GET", f"/emails/{ids[rcpt]}")[2]["last_event"],
                                 types[-1].split(".", 1)[1])
        bounce = self.events(ids["bounce@ext.test"])[-1]["payload"]["data"]
        self.assertEqual(bounce["bounce"]["type"], "Permanent")
        self.assertIn("reason", self.events(ids["fail@ext.test"])[0]["payload"]["data"]["failed"])

    def test_shuffle_duplicates_and_webhook_retries(self) -> None:
        self.ctl("POST", "/_mock/config", {"duplicate_webhooks": True, "shuffle_events": True})
        s, out = self.send(to="complain@ext.test")
        self.wait_idle()
        evs = self.events(out["id"])
        self.assertEqual(len(evs), 6)
        ids = [e["svix_id"] for e in evs]
        self.assertEqual(len(set(ids)), 3)
        self.assertTrue(all(ids.count(i) == 2 for i in ids))
        # retries: the receiver fails, the sender tries 3 times with the same svix-id
        self.ctl("POST", "/_mock/config", {"duplicate_webhooks": False, "shuffle_events": False})
        with _Receiver.lock:
            _Receiver.status = 500
            _Receiver.events.clear()
        self.ctl("POST", "/_mock/webhook", {"payload": {"type": "email.delivered", "created_at": "x",
                                                        "data": {"email_id": "e-1"}}})
        self.wait_idle()
        evs = self.events("e-1")
        self.assertEqual(len(evs), 3)
        self.assertEqual(len({e["svix_id"] for e in evs}), 1)
        log = self.ctl("GET", "/_mock/webhooks")["deliveries"]
        self.assertEqual([d["attempt"] for d in log if d["email_id"] == "e-1"], [1, 2, 3])
        # webhooks disabled: nothing is sent
        with _Receiver.lock:
            _Receiver.status = 200
            _Receiver.events.clear()
        self.ctl("POST", "/_mock/config", {"webhooks_enabled": False})
        self.send(to="ok@ext.test")
        self.wait_idle()
        self.assertEqual(self.events(), [])

    def test_meta_delay(self) -> None:
        self.ctl("POST", "/_mock/config", {"meta_delay": 2.0})
        s, out = self.send(to="ok@ext.test")
        self.assertIsNone(self.api("GET", f"/emails/{out['id']}")[2]["message_id"])
        self.wait_idle()
        self.assertTrue(all("message_id" not in e["payload"]["data"] for e in self.events(out["id"])))
        wait_until(lambda: self.api("GET", f"/emails/{out['id']}")[2]["message_id"], timeout=6)

    def test_schedule_patch_cancel_advance(self) -> None:
        at = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(time.time() + 3600))
        s, out = self.send(to="ok@ext.test", scheduled_at=at)
        self.assertEqual(s, 200, out)
        eid = out["id"]
        got = self.api("GET", f"/emails/{eid}")[2]
        self.assertEqual(got["last_event"], "scheduled")
        self.assertIsNone(got["message_id"])
        self.assertTrue(got["scheduled_at"].endswith("+00"))
        self.wait_idle()
        self.assertEqual([e["payload"]["type"] for e in self.events(eid)], ["email.scheduled"])
        at2 = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + 7200))
        s, _, r = self.api("PATCH", f"/emails/{eid}", {"scheduled_at": at2})
        self.assertEqual((s, r), (200, {"object": "email", "id": eid}))
        self.assertAlmostEqual(self.ctl("GET", f"/_mock/emails/{eid}")["scheduled_at"], parse_iso(at2), places=3)
        s, _, r = self.api("POST", f"/emails/{eid}/cancel")
        self.assertEqual(s, 200)
        self.assertEqual(self.api("GET", f"/emails/{eid}")[2]["last_event"], "canceled")
        self.assertEqual(self.api("POST", f"/emails/{eid}/cancel")[0], 422)
        self.assertEqual(self.api("PATCH", f"/emails/{eid}", {"scheduled_at": at2})[0], 422)
        # a second one fires when the mock clock is advanced
        s, out = self.send(to="bob@corp.test", scheduled_at=at)
        eid2 = out["id"]
        adv = self.ctl("POST", "/_mock/advance?seconds=3700")
        self.assertEqual(adv["fired"], [eid2])
        self.wait_idle()
        self.assertEqual(self.api("GET", f"/emails/{eid2}")[2]["last_event"], "delivered")
        self.assertEqual([e["payload"]["type"] for e in self.events(eid2)],
                         ["email.scheduled", "email.sent", "email.delivered"])
        self.assertEqual(len([e for e in self.events() if e["payload"]["type"] == "email.received"]), 1)
        self.assertEqual(self.api("POST", f"/emails/{eid2}/cancel")[0], 422)

    def test_inject_split_strip_and_pagination(self) -> None:
        self.ctl("POST", "/_mock/config", {"split_delivery": True})
        out = self.ctl("POST", "/_mock/inbound", {
            "from": "Partner <p@ext.test>", "to": ["alice@corp.test", "bob@corp.test"],
            "cc": ["support@corp.test"], "subject": "周报", "subject_charset": "gb18030",
            "text": "本周进度", "dmarc": "fail", "in_reply_to": "<x@corp.test>",
            "attachments": [{"filename": "a.txt", "text": "hello"}]})
        self.assertEqual(len(out["ids"]), 3)
        recs = [self.api("GET", f"/emails/receiving/{i}?html_format=cid")[2] for i in out["ids"]]
        self.assertEqual(sorted(r["received_for"][0] for r in recs),
                         ["alice@corp.test", "bob@corp.test", "support@corp.test"])
        self.assertEqual({r["message_id"] for r in recs}, {out["message_id"]})
        self.assertEqual(recs[0]["subject"], "周报")
        self.assertTrue(recs[0]["headers"]["subject"].lower().startswith("=?gb18030?"))
        self.assertEqual(recs[0]["authentication"]["dmarc"], "fail")
        self.assertIn("dmarc=fail", recs[0]["headers"]["authentication-results"])
        self.wait_idle()
        self.assertEqual(len(self.events()), 3)
        # strip_custom_headers on loopback
        self.ctl("POST", "/_mock/config", {"split_delivery": False, "strip_custom_headers": True})
        s, sent = self.send(to="alice@corp.test", headers={"X-AzMail-Ref": "u2", "In-Reply-To": "<a@b>"})
        self.wait_idle()
        rid = next(e for e in self.events() if e["payload"]["type"] == "email.received"
                   and e["payload"]["data"]["received_for"] == ["alice@corp.test"]
                   and e["payload"]["data"]["subject"] == "Hello")["payload"]["data"]["email_id"]
        rec = self.api("GET", f"/emails/receiving/{rid}")[2]
        self.assertNotIn("x-azmail-ref", rec["headers"])
        self.assertEqual(rec["headers"]["in-reply-to"], "<a@b>")
        # pagination (newest first)
        for i in range(3):
            self.ctl("POST", "/_mock/inbound", {"from": "p@ext.test", "to": "alice@corp.test",
                                                "subject": f"n{i}", "webhook": False})
        all_ids = [r["id"] for r in self.api("GET", "/emails/receiving?limit=100")[2]["data"]]
        self.assertEqual(len(all_ids), 7)
        self.assertEqual(self.api("GET", f"/emails/receiving/{all_ids[0]}")[2]["subject"], "n2")
        p1 = self.api("GET", "/emails/receiving?limit=3")[2]
        self.assertEqual(([r["id"] for r in p1["data"]], p1["has_more"]), (all_ids[:3], True))
        p2 = self.api("GET", f"/emails/receiving?limit=3&after={all_ids[2]}")[2]
        self.assertEqual([r["id"] for r in p2["data"]], all_ids[3:6])
        p3 = self.api("GET", f"/emails/receiving?limit=3&after={all_ids[5]}")[2]
        self.assertEqual(([r["id"] for r in p3["data"]], p3["has_more"]), (all_ids[6:], False))
        p4 = self.api("GET", f"/emails/receiving?limit=2&before={all_ids[4]}")[2]
        self.assertEqual(([r["id"] for r in p4["data"]], p4["has_more"]), (all_ids[2:4], True))
        for q in ("", "?limit=0", "?limit=101", "?limit=x", f"?limit=2&after={all_ids[0]}&before={all_ids[1]}",
                  "?limit=2&after=unknown"):
            with self.subTest(q=q):
                self.assertEqual(self.api("GET", "/emails/receiving" + q)[0], 422)

    def test_download_ttl_and_raw_missing(self) -> None:
        self.ctl("POST", "/_mock/config", {"download_ttl": 1, "raw_missing": True})
        out = self.ctl("POST", "/_mock/inbound", {"from": "p@ext.test", "to": "alice@corp.test",
                                                  "subject": "x", "attachments": [{"filename": "a.txt", "text": "hi"}]})
        rid = out["ids"][0]
        self.assertIsNone(self.api("GET", f"/emails/receiving/{rid}")[2]["raw"])
        att = self.api("GET", f"/emails/receiving/{rid}/attachments")[2]["data"][0]
        single = self.api("GET", f"/emails/receiving/{rid}/attachments/{att['id']}")[2]
        self.assertEqual(single["filename"], "a.txt")
        url = urllib.parse.urlsplit(att["download_url"])
        time.sleep(1.3)
        self.assertEqual(self.http("GET", f"{url.path}?{url.query}")[0], 403)

    def test_reset(self) -> None:
        self.send(to="ok@ext.test")
        self.ctl("POST", "/_mock/config", {"meta_delay": 5})
        self.ctl("POST", "/_mock/advance?seconds=100")
        self.ctl("POST", "/_mock/reset", {"keep_data": True})
        state = self.ctl("GET", "/_mock/state")
        self.assertEqual((state["offset"], state["config"]["meta_delay"], state["emails"]), (0.0, 0.0, 1))
        self.ctl("POST", "/_mock/reset")
        self.assertEqual(self.ctl("GET", "/_mock/state")["emails"], 0)
        status, _, _ = self.http("POST", "/_mock/config", {"nope": 1})
        self.assertEqual(status, 400)

    # -- S3 -----------------------------------------------------------------------------------------
    def s3(self, method: str, key: str, body: bytes | None = None, *, bucket: str = "azmail-test",
           payload_hash: str | None = None, extra: dict[str, str] | None = None,
           secret: str = DEFAULT_S3_SECRET_KEY, region: str = "auto", query: list | None = None):
        host = f"127.0.0.1:{self.s3_port}"
        raw_path = f"/{bucket}/{key}" if key else f"/{bucket}"
        amz = sigv4.amz_date()
        ph = payload_hash or (sigv4.sha256_hex(body) if body is not None else sigv4.EMPTY_SHA256)
        hdrs = [("host", host), ("x-amz-content-sha256", ph), ("x-amz-date", amz)]
        hdrs += [(k.lower(), v) for k, v in (extra or {}).items() if k.lower().startswith("x-amz-")]
        signed = sigv4.sign_request(DEFAULT_S3_ACCESS_KEY, secret, method, raw_path, query or [],
                                    hdrs, ph, amz, region)
        headers = {k: v for k, v in hdrs if k != "host"}
        headers.update(extra or {})
        headers["Authorization"] = signed.authorization
        path = sigv4.canonical_uri(raw_path)
        if query:
            path += "?" + sigv4.canonical_query(query)
        return self.http(method, path, body, headers, port=self.s3_port)

    def test_s3_object_lifecycle(self) -> None:
        data = b"blob bytes \x00\x01"
        sha = hashlib.sha256(data).hexdigest()
        key = f"azmail/blobs/{sha[:2]}/{sha[2:4]}/{sha}"
        status, hdrs, _ = self.s3("PUT", key, data, extra={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200)
        self.assertEqual(hdrs["etag"], '"' + hashlib.md5(data).hexdigest() + '"')  # noqa: S324
        status, _, body = self.s3("PUT", key, data)
        self.assertEqual(status, 429)
        self.assertIn(b"<Code>TooManyRequests</Code>", body)
        status, hdrs, body = self.s3("GET", key)
        self.assertEqual((status, body, hdrs["content-type"]), (200, data, "application/octet-stream"))
        self.assertNotIn("x-content-type-options", hdrs)
        status, hdrs, body = self.s3("HEAD", key)
        self.assertEqual((status, hdrs["content-length"], body), (200, str(len(data)), b""))
        status, hdrs, body = self.s3("GET", key, extra={"Range": "bytes=0-3"})
        self.assertEqual((status, body), (206, data[:4]))
        listing = self.ctl("GET", "/_mock/s3")
        self.assertEqual([(o["key"], o["sha256"]) for o in listing["objects"]], [(key, sha)])
        status, _, body = self.s3("GET", "", query=[("list-type", "2"), ("prefix", "azmail/")])
        self.assertIn(key.encode(), body)
        self.assertEqual(self.s3("DELETE", key)[0], 204)
        self.assertEqual(self.s3("DELETE", key)[0], 204)
        status, _, body = self.s3("GET", key)
        self.assertEqual(status, 404)
        self.assertIn(b"<Code>NoSuchKey</Code>", body)
        status, _, body = self.s3("HEAD", key)
        self.assertEqual((status, body), (404, b""))
        status, _, body = self.s3("GET", "x", bucket="nope")
        self.assertIn(b"<Code>NoSuchBucket</Code>", body)

    def test_s3_auth_errors(self) -> None:
        cases = [
            (dict(secret="wrong"), 403, "SignatureDoesNotMatch"),
            (dict(region="us-east-1"), 400, "AuthorizationHeaderMalformed"),
            (dict(payload_hash=sigv4.sha256_hex(b"other")), 400, "XAmzContentSHA256Mismatch"),
            (dict(payload_hash="STREAMING-AWS4-HMAC-SHA256-PAYLOAD"), 501, "NotImplemented"),
        ]
        for kw, status, code in cases:
            with self.subTest(code=code):
                got, _, body = self.s3("PUT", f"k-{code}", b"data", **kw)
                self.assertEqual(got, status, body)
                self.assertIn(f"<Code>{code}</Code>".encode(), body)
        status, _, body = self.http("GET", "/azmail-test/k", port=self.s3_port)
        self.assertEqual(status, 403)
        self.assertIn(b"<Code>AccessDenied</Code>", body)
        # unsigned x-amz-* header
        host = f"127.0.0.1:{self.s3_port}"
        amz = sigv4.amz_date()
        hdrs = [("host", host), ("x-amz-content-sha256", sigv4.EMPTY_SHA256), ("x-amz-date", amz)]
        signed = sigv4.sign_request(DEFAULT_S3_ACCESS_KEY, DEFAULT_S3_SECRET_KEY, "GET", "/azmail-test/k",
                                    [], hdrs, sigv4.EMPTY_SHA256, amz)
        status, _, body = self.http("GET", "/azmail-test/k", headers={
            "x-amz-content-sha256": sigv4.EMPTY_SHA256, "x-amz-date": amz, "x-amz-meta-z": "1",
            "Authorization": signed.authorization}, port=self.s3_port)
        self.assertEqual(status, 403)
        self.assertIn(b"HeadersNotSigned", body)
        # PUT without Content-Length -> 411
        ph = sigv4.sha256_hex(b"")
        signed = sigv4.sign_request(DEFAULT_S3_ACCESS_KEY, DEFAULT_S3_SECRET_KEY, "PUT", "/azmail-test/k",
                                    [], [("host", host), ("x-amz-content-sha256", ph), ("x-amz-date", amz)],
                                    ph, amz)
        with socket.create_connection(("127.0.0.1", self.s3_port), timeout=5) as sock:
            sock.sendall((f"PUT /azmail-test/k HTTP/1.1\r\nHost: {host}\r\nx-amz-content-sha256: {ph}\r\n"
                          f"x-amz-date: {amz}\r\nAuthorization: {signed.authorization}\r\n\r\n").encode())
            resp = b""
            while b"</Error>" not in resp:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                resp += chunk
        self.assertTrue(resp.startswith(b"HTTP/1.1 411"), resp[:60])
        self.assertIn(b"MissingContentLength", resp)

    def test_s3_presigned(self) -> None:
        data = b"<svg xmlns='http://www.w3.org/2000/svg'/>"
        self.assertEqual(self.s3("PUT", "obj.svg", data)[0], 200)
        host = f"127.0.0.1:{self.s3_port}"

        def presigned(expires: int = 60, date: float | None = None, method: str = "GET") -> str:
            p = sigv4.presign(DEFAULT_S3_ACCESS_KEY, DEFAULT_S3_SECRET_KEY, method=method, scheme="http",
                              host=host, raw_path="/azmail-test/obj.svg",
                              extra_query=[("response-content-type", "application/octet-stream"),
                                           ("response-content-disposition",
                                            "attachment; filename*=UTF-8''%E5%9B%BE.svg")],
                              amz_date_value=sigv4.amz_date(date), expires=expires)
            u = urllib.parse.urlsplit(p.url)
            return f"{u.path}?{u.query}"

        status, hdrs, body = self.http("GET", presigned(), port=self.s3_port)
        self.assertEqual((status, body), (200, data))
        self.assertEqual(hdrs["content-type"], "application/octet-stream")
        self.assertEqual(hdrs["content-disposition"], "attachment; filename*=UTF-8''%E5%9B%BE.svg")
        self.assertEqual(self.http("HEAD", presigned(method="HEAD"), port=self.s3_port)[0], 200)
        url = presigned()
        tampered = url.replace("response-content-type=application%2Foctet-stream",
                               "response-content-type=text%2Fhtml")
        status, _, body = self.http("GET", tampered, port=self.s3_port)
        self.assertEqual(status, 403)
        self.assertIn(b"SignatureDoesNotMatch", body)
        sig_idx = url.index("X-Amz-Signature=") + len("X-Amz-Signature=")
        flipped = url[:sig_idx] + ("0" if url[sig_idx] != "0" else "1") + url[sig_idx + 1:]
        self.assertEqual(self.http("GET", flipped, port=self.s3_port)[0], 403)
        status, _, body = self.http("GET", presigned(expires=1, date=time.time() - 5), port=self.s3_port)
        self.assertEqual(status, 403)
        self.assertIn(b"Request has expired", body)
        # a presigned GET does not authorize a PUT
        status, _, body = self.http("PUT", url, b"x", port=self.s3_port)
        self.assertEqual(status, 403)

    def test_s3_faults(self) -> None:
        self.ctl("POST", "/_mock/faults", [{"match": "PUT /azmail-test/*", "target": "s3",
                                            "status": 503, "name": "ServiceUnavailable"}])
        status, _, body = self.s3("PUT", "k", b"x")
        self.assertEqual(status, 503)
        self.assertIn(b"<Code>ServiceUnavailable</Code>", body)
        self.assertEqual(self.s3("PUT", "k", b"x")[0], 200)


class S3PersistenceTests(unittest.TestCase):
    """--s3-data-dir: objects survive a restart of the mock; DELETE and reset(s3) remove them."""

    @staticmethod
    def _signed(port: int, method: str, key: str, body: bytes | None = None) -> tuple[int, bytes]:
        host = f"127.0.0.1:{port}"
        amz = sigv4.amz_date()
        ph = sigv4.sha256_hex(body) if body is not None else sigv4.EMPTY_SHA256
        hdrs = [("host", host), ("x-amz-content-sha256", ph), ("x-amz-date", amz)]
        signed = sigv4.sign_request(DEFAULT_S3_ACCESS_KEY, DEFAULT_S3_SECRET_KEY, method,
                                    f"/dev-bucket/{key}", [], hdrs, ph, amz)
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        headers = {k: v for k, v in hdrs if k != "host"}
        headers["Authorization"] = signed.authorization
        conn.request(method, sigv4.canonical_uri(f"/dev-bucket/{key}"), body=body, headers=headers)
        resp = conn.getresponse()
        out = resp.status, resp.read()
        conn.close()
        return out

    def test_restart_keeps_objects(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            first = MockResend(s3_buckets=["dev-bucket"], s3_data_dir=tmp, rate_limit=0)
            _, port = first.serve("127.0.0.1", 0, 0)
            assert port is not None
            self.assertEqual(self._signed(port, "PUT", "azmail/blobs/aa/bb/keep", b"persist me")[0], 200)
            self.assertEqual(self._signed(port, "PUT", "azmail/blobs/aa/bb/gone", b"x")[0], 200)
            self.assertEqual(self._signed(port, "DELETE", "azmail/blobs/aa/bb/gone")[0], 204)
            first.shutdown()
            second = MockResend(s3_buckets=["dev-bucket"], s3_data_dir=tmp, rate_limit=0)
            _, port2 = second.serve("127.0.0.1", 0, 0)
            assert port2 is not None
            try:
                self.assertEqual(self._signed(port2, "GET", "azmail/blobs/aa/bb/keep"), (200, b"persist me"))
                self.assertEqual(self._signed(port2, "GET", "azmail/blobs/aa/bb/gone")[0], 404)
            finally:
                second.shutdown()


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    verbosity = 2 if "-v" in argv else 1
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=verbosity).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
