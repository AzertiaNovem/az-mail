"""Minimal Cloudflare R2 / S3 endpoint (DESIGN Addendum A, A.2).

Path-style ``/<bucket>/<key>``; ``PUT`` / ``GET`` / ``HEAD`` / ``DELETE`` objects, ``HEAD`` /
``GET`` (ListObjectsV2) buckets. Every request must be SigV4-authenticated — Authorization header
or presigned query — and is verified exactly like S3 (see sigv4.py). R2 specifics emulated:
region ``auto``; ``Content-Length`` required on PUT (411 otherwise, no chunked / streaming
SigV4); at most one write per key per second (429 TooManyRequests); S3 XML errors;
``response-content-type`` / ``response-content-disposition`` (and the other ``response-*``)
overrides on GET/HEAD; no security headers (no nosniff).
"""

from __future__ import annotations

import base64
import email.utils
import hashlib
import json
import os
import secrets
import shutil
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from typing import TYPE_CHECKING, Any, Iterator
from xml.sax.saxutils import escape

from . import sigv4
from .store import S3Object

if TYPE_CHECKING:  # pragma: no cover
    from .server import MockResend

RESPONSE_OVERRIDES = {
    "response-content-type": "Content-Type",
    "response-content-language": "Content-Language",
    "response-expires": "Expires",
    "response-cache-control": "Cache-Control",
    "response-content-disposition": "Content-Disposition",
    "response-content-encoding": "Content-Encoding",
}


class S3Disk:
    """Optional write-through persistence (``--s3-data-dir``) so dev blobs survive restarts.

    Layout: ``<root>/<bucket>/<base64url(key)>`` (bytes) + ``….meta.json`` (type, etag, …).
    """

    def __init__(self, root: str | os.PathLike[str]) -> None:
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def _path(self, bucket: str, key: str) -> Path:
        name = base64.urlsafe_b64encode(key.encode("utf-8")).decode("ascii").rstrip("=")
        return self.root / bucket / name

    def save(self, bucket: str, key: str, obj: S3Object) -> None:
        path = self._path(bucket, key)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_bytes(obj.data)
        os.replace(tmp, path)
        meta = {"key": key, "content_type": obj.content_type, "etag": obj.etag,
                "last_modified": obj.last_modified, "metadata": obj.metadata}
        path.with_name(path.name + ".meta.json").write_text(json.dumps(meta), encoding="utf-8")

    def delete(self, bucket: str, key: str) -> None:
        path = self._path(bucket, key)
        for p in (path, path.with_name(path.name + ".meta.json")):
            try:
                p.unlink()
            except FileNotFoundError:
                pass

    def load(self) -> Iterator[tuple[str, str, S3Object]]:
        for meta_path in sorted(self.root.glob("*/*.meta.json")):
            data_path = meta_path.with_name(meta_path.name[: -len(".meta.json")])
            if not data_path.exists():
                continue
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            yield meta_path.parent.name, meta["key"], S3Object(
                data=data_path.read_bytes(), content_type=meta["content_type"], etag=meta["etag"],
                last_modified=float(meta["last_modified"]), metadata=dict(meta.get("metadata") or {}))

    def clear(self) -> None:
        for child in self.root.iterdir():
            if child.is_dir():
                shutil.rmtree(child, ignore_errors=True)


def parse_query(qs: str) -> list[tuple[str, str]]:
    """Decoded (name, value) pairs in order. Like S3, '+' in the query decodes to a space."""
    out: list[tuple[str, str]] = []
    if not qs:
        return out
    for piece in qs.split("&"):
        if not piece:
            continue
        name, _, value = piece.partition("=")
        out.append((urllib.parse.unquote_plus(name), urllib.parse.unquote_plus(value)))
    return out


def error_xml(code: str, message: str, resource: str, request_id: str,
              extra: dict[str, str] | None = None) -> bytes:
    parts = [f"<Code>{escape(code)}</Code>", f"<Message>{escape(message)}</Message>"]
    for k, v in (extra or {}).items():
        parts.append(f"<{k}>{escape(str(v))}</{k}>")
    parts.append(f"<Resource>{escape(resource)}</Resource>")
    parts.append(f"<RequestId>{request_id}</RequestId>")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<Error>' + "".join(parts) + "</Error>").encode()


class S3Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockR2/1.0"
    mock: "MockResend"  # injected by the server factory

    def version_string(self) -> str:  # R2 answers as "Server: cloudflare"
        return "cloudflare"

    def log_message(self, fmt: str, *args: Any) -> None:  # quiet; the request log is in state
        if self.mock.verbose:
            super().log_message(fmt, *args)

    # BaseHTTPRequestHandler dispatch
    def do_GET(self) -> None:
        self._handle("GET")

    def do_HEAD(self) -> None:
        self._handle("HEAD")

    def do_PUT(self) -> None:
        self._handle("PUT")

    def do_DELETE(self) -> None:
        self._handle("DELETE")

    def do_POST(self) -> None:
        self._handle("POST")

    # ------------------------------------------------------------------------------------------
    def _send(self, status: int, body: bytes = b"", headers: dict[str, str] | None = None,
              method: str = "GET") -> None:
        self.send_response(status)
        hdrs = {"x-amz-request-id": self._request_id}
        hdrs.update(headers or {})
        if "Content-Length" not in hdrs:
            hdrs["Content-Length"] = str(len(body))
        for k, v in hdrs.items():
            self.send_header(k, v)
        self.end_headers()
        if method != "HEAD" and body:
            self.wfile.write(body)

    def _error(self, status: int, code: str, message: str, method: str,
               extra: dict[str, str] | None = None) -> None:
        body = error_xml(code, message, self._raw_path, self._request_id, extra)
        if method == "HEAD":  # S3 sends no body for HEAD errors
            self._send(status, b"", {"Content-Length": "0"}, method="HEAD")
        else:
            self._send(status, body, {"Content-Type": "application/xml"}, method)
        self._log["status"] = status
        self._log["code"] = code

    def _handle(self, method: str) -> None:
        self._request_id = secrets.token_hex(8).upper()
        state = self.mock.state
        split = urllib.parse.urlsplit(self.path)
        self._raw_path = urllib.parse.unquote(split.path)
        query = parse_query(split.query)
        qnames = {k for k, _ in query}
        auth_mode = ("presigned" if "X-Amz-Signature" in qnames or "X-Amz-Algorithm" in qnames
                     else "header" if self.headers.get("Authorization") else "none")
        self._log = {"seq": state.next_seq(), "at": time.time(), "method": method,
                     "path": self._raw_path, "auth": auth_mode, "status": None, "code": None,
                     "user_agent": self.headers.get("User-Agent", "")}
        with state.lock:
            state.s3_requests.append(self._log)

        # Body (PUT): Content-Length is mandatory (R2: no chunked uploads).
        body = b""
        if method == "PUT":
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked" or \
                    self.headers.get("Content-Length") is None:
                self.close_connection = True
                return self._error(411, "MissingContentLength",
                                   "You must provide the Content-Length HTTP header.", method)
            try:
                length = int(self.headers["Content-Length"])
            except ValueError:
                self.close_connection = True
                return self._error(400, "InvalidArgument", "Invalid Content-Length", method)
            if length > self.mock.s3_max_object_bytes:
                self.close_connection = True
                return self._error(400, "EntityTooLarge",
                                   "Your proposed upload exceeds the maximum allowed object size.",
                                   method)
            body = self._read_exact(length)
            if body is None:
                return
        else:
            length = int(self.headers.get("Content-Length") or 0)
            if length:
                if self._read_exact(length) is None:
                    return

        fault = state.take_fault("s3", method, self._raw_path)
        if fault is not None:
            if fault.close:
                self.close_connection = True
                self._log["status"] = "closed"
                return
            if fault.delay:
                time.sleep(fault.delay)
            if fault.status:
                return self._error(fault.status, fault.name or "InternalError",
                                   fault.message or "Injected fault", method)

        try:
            sigv4.verify(method, self._raw_path, query, list(self.headers.items()),
                         self.mock.s3_keys, now=time.time(), regions=self.mock.s3_regions,
                         body_sha256=hashlib.sha256(body).hexdigest() if method == "PUT" else None)
        except sigv4.AuthFailure as failure:
            self._log["auth_ok"] = False
            return self._error(failure.status, failure.code, failure.message, method, failure.extra)
        self._log["auth_ok"] = True

        segments = self._raw_path.lstrip("/").split("/", 1)
        bucket = segments[0] if segments and segments[0] else ""
        key = segments[1] if len(segments) > 1 else ""
        if not bucket:
            if method == "GET":
                return self._list_buckets()
            return self._error(405, "MethodNotAllowed", "The specified method is not allowed.", method)
        with state.lock:
            known = bucket in state.buckets
        if not known:
            return self._error(404, "NoSuchBucket", "The specified bucket does not exist.", method,
                               {"BucketName": bucket})
        if not key:
            if method == "HEAD":
                self._log["status"] = 200
                return self._send(200, b"", {"Content-Length": "0"}, method)
            if method == "GET":
                return self._list_objects(bucket, dict(query))
            return self._error(405, "MethodNotAllowed", "The specified method is not allowed.", method)

        if method == "PUT":
            return self._put(bucket, key, body)
        if method in ("GET", "HEAD"):
            return self._get(bucket, key, method, dict(query))
        if method == "DELETE":
            with state.lock:
                state.objects.pop((bucket, key), None)
                if self.mock.s3_disk is not None:
                    self.mock.s3_disk.delete(bucket, key)
            self._log["status"] = 204
            return self._send(204, b"", {"Content-Length": "0"}, method)
        return self._error(405, "MethodNotAllowed", "The specified method is not allowed.", method)

    def _read_exact(self, length: int) -> bytes | None:
        chunks: list[bytes] = []
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 1 << 20))
            if not chunk:
                self.close_connection = True
                self._log["status"] = "incomplete_body"
                return None
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _put(self, bucket: str, key: str, body: bytes) -> None:
        state = self.mock.state
        now = time.time()
        with state.lock:
            last = state.last_write.get((bucket, key))
            if last is not None and now - last < self.mock.s3_write_interval:
                return self._error(429, "TooManyRequests",
                                   "Reduce your concurrent request rate for the same object.", "PUT")
            meta = {k[len("x-amz-meta-"):]: v for k, v in self.headers.items()
                    if k.lower().startswith("x-amz-meta-")}
            etag = '"' + hashlib.md5(body).hexdigest() + '"'  # noqa: S324 (S3 ETag semantics)
            obj = S3Object(data=body, content_type=self.headers.get("Content-Type") or "binary/octet-stream",
                           etag=etag, last_modified=now, metadata=meta)
            state.objects[(bucket, key)] = obj
            state.last_write[(bucket, key)] = now
            if self.mock.s3_disk is not None:
                self.mock.s3_disk.save(bucket, key, obj)
        self._log["status"] = 200
        self._send(200, b"", {"ETag": etag, "Content-Length": "0"}, "PUT")

    def _get(self, bucket: str, key: str, method: str, query: dict[str, str]) -> None:
        state = self.mock.state
        with state.lock:
            obj = state.objects.get((bucket, key))
        if obj is None:
            return self._error(404, "NoSuchKey", "The specified key does not exist.", method,
                               {"Key": key})
        headers = {
            "Content-Type": obj.content_type,
            "ETag": obj.etag,
            "Last-Modified": email.utils.formatdate(obj.last_modified, usegmt=True),
            "Accept-Ranges": "bytes",
        }
        for k, v in obj.metadata.items():
            headers["x-amz-meta-" + k] = v
        for param, header in RESPONSE_OVERRIDES.items():
            if param in query:
                headers[header] = query[param]
        data = obj.data
        status = 200
        rng = self.headers.get("Range")
        if rng:
            parsed = _parse_range(rng, len(data))
            if parsed is None:
                headers["Content-Range"] = f"bytes */{len(data)}"
                return self._error(416, "InvalidRange", "The requested range is not satisfiable",
                                   method)
            start, end = parsed
            headers["Content-Range"] = f"bytes {start}-{end}/{len(data)}"
            data = data[start:end + 1]
            status = 206
        headers["Content-Length"] = str(len(data))
        self._log["status"] = status
        self._send(status, data, headers, method)

    def _list_buckets(self) -> None:
        state = self.mock.state
        with state.lock:
            names = sorted(state.buckets)
        items = "".join(f"<Bucket><Name>{escape(n)}</Name><CreationDate>2026-01-01T00:00:00.000Z"
                        f"</CreationDate></Bucket>" for n in names)
        body = ('<?xml version="1.0" encoding="UTF-8"?>\n<ListAllMyBucketsResult><Buckets>'
                f"{items}</Buckets><Owner><ID>mock</ID></Owner></ListAllMyBucketsResult>").encode()
        self._log["status"] = 200
        self._send(200, body, {"Content-Type": "application/xml"})

    def _list_objects(self, bucket: str, query: dict[str, str]) -> None:
        state = self.mock.state
        prefix = query.get("prefix", "")
        try:
            max_keys = max(0, min(1000, int(query.get("max-keys", "1000"))))
        except ValueError:
            return self._error(400, "InvalidArgument", "max-keys must be an integer", "GET")
        start_after = query.get("start-after") or query.get("continuation-token") or ""
        with state.lock:
            keys = sorted(k for (b, k) in state.objects if b == bucket and k.startswith(prefix)
                          and k > start_after)
            selected = [(k, state.objects[(bucket, k)]) for k in keys[:max_keys]]
        truncated = len(keys) > max_keys
        contents = "".join(
            f"<Contents><Key>{escape(k)}</Key><Size>{len(o.data)}</Size><ETag>{escape(o.etag)}</ETag>"
            f"<LastModified>{time.strftime('%Y-%m-%dT%H:%M:%S.000Z', time.gmtime(o.last_modified))}"
            f"</LastModified></Contents>" for k, o in selected)
        token = f"<NextContinuationToken>{escape(selected[-1][0])}</NextContinuationToken>" \
            if truncated and selected else ""
        body = ('<?xml version="1.0" encoding="UTF-8"?>\n<ListBucketResult>'
                f"<Name>{escape(bucket)}</Name><Prefix>{escape(prefix)}</Prefix>"
                f"<KeyCount>{len(selected)}</KeyCount><MaxKeys>{max_keys}</MaxKeys>"
                f"<IsTruncated>{'true' if truncated else 'false'}</IsTruncated>{token}{contents}"
                "</ListBucketResult>").encode()
        self._log["status"] = 200
        self._send(200, body, {"Content-Type": "application/xml"})


def _parse_range(value: str, size: int) -> tuple[int, int] | None:
    if not value.startswith("bytes=") or "," in value or size == 0:
        return None
    spec = value[len("bytes="):].strip()
    first, _, last = spec.partition("-")
    try:
        if first == "":
            n = int(last)
            if n <= 0:
                return None
            return max(0, size - n), size - 1
        start = int(first)
        end = int(last) if last else size - 1
    except ValueError:
        return None
    if start >= size or end < start:
        return None
    return start, min(end, size - 1)
