"""Small urllib-based REST client for the AZ Mail API (docs/API.md + Addendum B).

* Never follows redirects (302 to presigned R2 URLs is asserted explicitly).
* ``expect=`` turns status mismatches into ApiError (an AssertionError, so ``eventually``
  retries it) carrying the method, path, status and error body.
"""

from __future__ import annotations

import json
import socket
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Iterable, Mapping

USER_AGENT = "azmail-e2e/1.0"


class ApiError(AssertionError):
    def __init__(self, method: str, url: str, resp: "Response", expected: Iterable[int]) -> None:
        self.resp = resp
        exp = "/".join(str(e) for e in expected)
        super().__init__(f"{method} {_redact(url)} → {resp.status} (expected {exp}): {resp.short()}")


def _redact(url: str) -> str:
    """Hides signed-URL query strings in messages (sig/exp/X-Amz-Signature)."""
    parts = urllib.parse.urlsplit(url)
    if not parts.query:
        return url
    q = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
    red = [(k, "…" if k.lower() in ("sig", "x-amz-signature", "x-amz-credential") else v) for k, v in q]
    return urllib.parse.urlunsplit(parts._replace(query=urllib.parse.urlencode(red)))


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes
    url: str

    def header(self, name: str, default: str | None = None) -> str | None:
        return self.headers.get(name.lower(), default)

    @property
    def text(self) -> str:
        return self.body.decode("utf-8", "replace")

    def json(self) -> Any:
        if not self.body:
            return None
        return json.loads(self.body)

    @property
    def error_code(self) -> str | None:
        try:
            data = self.json()
        except ValueError:
            return None
        if isinstance(data, dict) and isinstance(data.get("error"), dict):
            return data["error"].get("code")
        return None

    @property
    def error_details(self) -> dict[str, Any]:
        try:
            return (self.json() or {}).get("error", {}).get("details") or {}
        except (ValueError, AttributeError):
            return {}

    def short(self, limit: int = 400) -> str:
        text = self.text
        return text if len(text) <= limit else text[:limit] + "…"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D401, ANN001
        return None


_OPENER = urllib.request.build_opener(_NoRedirect())


def http_request(method: str, url: str, *, data: bytes | None = None,
                 headers: Mapping[str, str] | None = None, timeout: float = 30.0) -> Response:
    hdrs = {"User-Agent": USER_AGENT}
    hdrs.update(headers or {})
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with _OPENER.open(req, timeout=timeout) as resp:
            body = resp.read()
            return Response(resp.status, _lower(resp.headers.items()), body, url)
    except urllib.error.HTTPError as err:
        try:
            body = err.read() if err.fp is not None else b""
        finally:
            err.close()
        return Response(err.code, _lower(err.headers.items() if err.headers else []), body, url)


def _lower(items: Iterable[tuple[str, str]]) -> dict[str, str]:
    out: dict[str, str] = {}
    for k, v in items:
        k = k.lower()
        out[k] = f"{out[k]}, {v}" if k in out else v
    return out


class Api:
    def __init__(self, base: str, token: str | None = None, origin: str | None = None,
                 timeout: float = 30.0) -> None:
        self.base = base.rstrip("/")
        self.token = token
        self.origin = origin
        self.timeout = timeout

    def with_token(self, token: str | None) -> "Api":
        return Api(self.base, token, self.origin, self.timeout)

    def url(self, path: str, query: Mapping[str, Any] | None = None) -> str:
        url = path if path.startswith("http") else self.base + path
        if query:
            q = {k: v for k, v in query.items() if v is not None}
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(q, quote_via=urllib.parse.quote)
        return url

    def request(self, method: str, path: str, json_body: Any = None, *, data: bytes | None = None,
                headers: Mapping[str, str] | None = None, query: Mapping[str, Any] | None = None,
                expect: int | Iterable[int] | None = None, auth: bool = True) -> Response:
        hdrs: dict[str, str] = {}
        if auth and self.token:
            hdrs["Authorization"] = f"Bearer {self.token}"
        if self.origin:
            hdrs["Origin"] = self.origin
        if json_body is not None:
            data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
            hdrs["Content-Type"] = "application/json"
        hdrs.update(headers or {})
        url = self.url(path, query)
        resp = http_request(method, url, data=data, headers=hdrs, timeout=self.timeout)
        if expect is not None:
            allowed = (expect,) if isinstance(expect, int) else tuple(expect)
            if resp.status not in allowed:
                raise ApiError(method, url, resp, allowed)
        return resp

    # shortcuts returning parsed JSON when `expect` is given ------------------------------------
    def get(self, path: str, expect: int | Iterable[int] | None = 200, **kw: Any) -> Any:
        resp = self.request("GET", path, expect=expect, **kw)
        return resp.json() if expect is not None else resp

    def post(self, path: str, body: Any = None, expect: int | Iterable[int] | None = 200, **kw: Any) -> Any:
        resp = self.request("POST", path, body, expect=expect, **kw)
        return resp.json() if expect is not None else resp

    def put(self, path: str, body: Any = None, expect: int | Iterable[int] | None = 200, **kw: Any) -> Any:
        resp = self.request("PUT", path, body, expect=expect, **kw)
        return resp.json() if expect is not None else resp

    def patch(self, path: str, body: Any = None, expect: int | Iterable[int] | None = 200, **kw: Any) -> Any:
        resp = self.request("PATCH", path, body, expect=expect, **kw)
        return resp.json() if expect is not None else resp

    def delete(self, path: str, expect: int | Iterable[int] | None = 204, **kw: Any) -> Response:
        return self.request("DELETE", path, expect=expect, **kw)

    def call(self, method: str, path: str, body: Any = None, **kw: Any) -> Response:
        """Raw call without status expectation."""
        return self.request(method, path, body, **kw)

    def login(self, email: str, password: str) -> tuple["Api", dict[str, Any]]:
        data = self.request("POST", "/api/auth/login", {"email": email, "password": password},
                            expect=200, auth=False).json()
        return self.with_token(data["token"]), data


def raw_request(host: str, port: int, method: str, path: str, headers: Mapping[str, str],
                body: bytes = b"", timeout: float = 10.0) -> tuple[int, dict[str, str], bytes]:
    """Hand-written HTTP/1.1 request (for header-only oversize probes, e.g. 413 before body).

    Sends the request line + headers (+ ``body`` if given) and reads until the server closes or
    a full response (by Content-Length) arrived.
    """
    lines = [f"{method} {path} HTTP/1.1", f"Host: {host}:{port}", f"User-Agent: {USER_AGENT}"]
    lines += [f"{k}: {v}" for k, v in headers.items()]
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode() + body
    with socket.create_connection((host, port), timeout=timeout) as sock:
        try:
            sock.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        data = b""
        while True:
            try:
                chunk = sock.recv(65536)
            except (ConnectionResetError, socket.timeout):
                break
            if not chunk:
                break
            data += chunk
            head, sep, rest = data.partition(b"\r\n\r\n")
            if sep:
                hdrs = _parse_head(head)[1]
                length = int(hdrs.get("content-length", "-1"))
                if length >= 0 and len(rest) >= length:
                    break
    if not data:
        raise AssertionError(f"{method} {path}: no response (connection closed)")
    head, _, rest = data.partition(b"\r\n\r\n")
    status, hdrs = _parse_head(head)
    return status, hdrs, rest


def _parse_head(head: bytes) -> tuple[int, dict[str, str]]:
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    hdrs: dict[str, str] = {}
    for line in lines[1:]:
        k, _, v = line.partition(":")
        hdrs[k.strip().lower()] = v.strip()
    return status, hdrs
