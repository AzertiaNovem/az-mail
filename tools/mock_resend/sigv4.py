"""Pure AWS Signature Version 4 (S3 flavour): signing and server-side verification.

Mirrors DESIGN Addendum A.2 exactly so the mock R2 endpoint verifies requests the way S3/R2
does (and so the self-test can prove the algorithm against the AWS published S3 examples):

* URI-encode everything except ``A-Za-z0-9-._~`` with uppercase hex; space -> ``%20``; ``/`` is
  kept in the path and encoded as ``%2F`` in query values; query parameters are sorted after
  encoding; the path is not normalized.
* Canonical request lines are joined with ``\\n``; the header block ends with ``\\n`` (so a blank
  line precedes the signed-header list); there is no trailing newline.

No I/O and no clock: verification takes ``now`` (epoch seconds) explicitly.
"""

from __future__ import annotations

import calendar
import hashlib
import hmac
import re
import time
from dataclasses import dataclass, field
from typing import Iterable, Mapping, Sequence

ALGORITHM = "AWS4-HMAC-SHA256"
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD"
MAX_PRESIGN_EXPIRES = 604800  # 7 days (S3 and R2 limit)
MAX_CLOCK_SKEW = 15 * 60  # S3 rejects header-signed requests more than 15 min off

_UNRESERVED = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
_AMZ_DATE_RE = re.compile(r"^(\d{8})T(\d{6})Z$")
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")


# ---------------------------------------------------------------------------------------------
# building blocks
# ---------------------------------------------------------------------------------------------

def uri_encode(value: str | bytes, encode_slash: bool = True) -> str:
    """RFC 3986 percent-encoding as S3 SigV4 requires (uppercase hex, UTF-8 bytes)."""
    data = value.encode("utf-8") if isinstance(value, str) else value
    out = []
    for byte in data:
        if byte in _UNRESERVED or (byte == 0x2F and not encode_slash):
            out.append(chr(byte))
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def canonical_uri(raw_path: str) -> str:
    """Canonical URI for a RAW (decoded) path; empty -> ``/``."""
    if not raw_path:
        return "/"
    return uri_encode(raw_path, encode_slash=False)


def canonical_query(params: Iterable[tuple[str, str]]) -> str:
    """Canonical query string from RAW name/value pairs (encoded, then sorted)."""
    encoded = sorted((uri_encode(k), uri_encode(v)) for k, v in params)
    return "&".join(f"{k}={v}" for k, v in encoded)


def _trimall(value: str) -> str:
    return " ".join(str(value).strip().split())


def canonical_headers(headers: Iterable[tuple[str, str]]) -> tuple[str, str]:
    """(header block, signed-headers list). Every given header is signed; duplicates joined by ','."""
    merged: dict[str, list[str]] = {}
    for name, value in headers:
        merged.setdefault(name.strip().lower(), []).append(_trimall(value))
    names = sorted(merged)
    block = "".join(f"{n}:{','.join(merged[n])}\n" for n in names)
    return block, ";".join(names)


def canonical_request(method: str, c_uri: str, c_query: str, header_block: str,
                      signed_headers: str, payload_hash: str) -> str:
    return "\n".join([method, c_uri, c_query, header_block, signed_headers, payload_hash])


def credential_scope(date8: str, region: str, service: str) -> str:
    return f"{date8}/{region}/{service}/aws4_request"


def sha256_hex(data: str | bytes) -> str:
    return hashlib.sha256(data.encode("utf-8") if isinstance(data, str) else data).hexdigest()


def string_to_sign(amz_date: str, scope: str, creq: str) -> str:
    return "\n".join([ALGORITHM, amz_date, scope, sha256_hex(creq)])


def _hmac(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def signing_key(secret_access_key: str, date8: str, region: str, service: str) -> bytes:
    k_date = _hmac(("AWS4" + secret_access_key).encode("utf-8"), date8)
    k_region = _hmac(k_date, region)
    k_service = _hmac(k_region, service)
    return _hmac(k_service, "aws4_request")


def signature(key: bytes, sts: str) -> str:
    return hmac.new(key, sts.encode("utf-8"), hashlib.sha256).hexdigest()


def authorization_header(access_key_id: str, scope: str, signed_headers: str, sig: str) -> str:
    return (f"{ALGORITHM} Credential={access_key_id}/{scope}, "
            f"SignedHeaders={signed_headers}, Signature={sig}")


def amz_date(epoch_seconds: float | None = None) -> str:
    t = time.gmtime(time.time() if epoch_seconds is None else epoch_seconds)
    return time.strftime("%Y%m%dT%H%M%SZ", t)


def parse_amz_date(value: str) -> float | None:
    """Epoch seconds for ``YYYYMMDDTHHMMSSZ`` (None when malformed)."""
    if not _AMZ_DATE_RE.match(value or ""):
        return None
    try:
        return float(calendar.timegm(time.strptime(value, "%Y%m%dT%H%M%SZ")))
    except ValueError:
        return None


# ---------------------------------------------------------------------------------------------
# one-shot signing (used by the self-test, the E2E harness and tools that talk to the mock)
# ---------------------------------------------------------------------------------------------

@dataclass
class Signed:
    canonical_request: str
    string_to_sign: str
    signature: str
    signed_headers: str
    authorization: str


def sign_request(access_key_id: str, secret_access_key: str, method: str, raw_path: str,
                 query: Sequence[tuple[str, str]], headers: Sequence[tuple[str, str]],
                 payload_hash: str, amz_date_value: str, region: str = "auto",
                 service: str = "s3") -> Signed:
    """Header-auth signature. ``headers`` must include host, x-amz-date and x-amz-content-sha256."""
    block, signed = canonical_headers(headers)
    creq = canonical_request(method, canonical_uri(raw_path), canonical_query(query), block, signed,
                             payload_hash)
    scope = credential_scope(amz_date_value[:8], region, service)
    sts = string_to_sign(amz_date_value, scope, creq)
    sig = signature(signing_key(secret_access_key, amz_date_value[:8], region, service), sts)
    return Signed(creq, sts, sig, signed, authorization_header(access_key_id, scope, signed, sig))


@dataclass
class Presigned:
    url: str
    canonical_request: str
    signature: str


def presign(access_key_id: str, secret_access_key: str, *, method: str = "GET",
            scheme: str = "https", host: str, raw_path: str,
            extra_query: Sequence[tuple[str, str]] = (), amz_date_value: str,
            expires: int = 300, region: str = "auto", service: str = "s3") -> Presigned:
    """Presigned URL (query auth, SignedHeaders=host, UNSIGNED-PAYLOAD)."""
    if not 1 <= int(expires) <= MAX_PRESIGN_EXPIRES:
        raise ValueError("expires must be within 1..604800 seconds")
    scope = credential_scope(amz_date_value[:8], region, service)
    query = list(extra_query) + [
        ("X-Amz-Algorithm", ALGORITHM),
        ("X-Amz-Credential", f"{access_key_id}/{scope}"),
        ("X-Amz-Date", amz_date_value),
        ("X-Amz-Expires", str(int(expires))),
        ("X-Amz-SignedHeaders", "host"),
    ]
    c_query = canonical_query(query)
    block, signed = canonical_headers([("host", host)])
    creq = canonical_request(method, canonical_uri(raw_path), c_query, block, signed,
                             UNSIGNED_PAYLOAD)
    sts = string_to_sign(amz_date_value, scope, creq)
    sig = signature(signing_key(secret_access_key, amz_date_value[:8], region, service), sts)
    url = f"{scheme}://{host}{canonical_uri(raw_path)}?{c_query}&X-Amz-Signature={sig}"
    return Presigned(url, creq, sig)


# ---------------------------------------------------------------------------------------------
# server-side verification
# ---------------------------------------------------------------------------------------------

class AuthFailure(Exception):
    """An S3 authentication error (rendered as an S3 XML <Error> by the caller)."""

    def __init__(self, status: int, code: str, message: str, **extra: str) -> None:
        super().__init__(f"{code}: {message}")
        self.status = status
        self.code = code
        self.message = message
        self.extra = {k: v for k, v in extra.items() if v is not None}


@dataclass
class AuthResult:
    mode: str  # "header" | "presigned"
    access_key_id: str
    payload_hash: str  # claimed payload hash (hex, UNSIGNED-PAYLOAD)
    expires_at: float | None = None  # presigned only
    signed_headers: list[str] = field(default_factory=list)


def _header_lookup(headers: Sequence[tuple[str, str]]) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for name, value in headers:
        out.setdefault(name.lower(), []).append(value)
    return out


def _parse_credential(cred: str, keys: Mapping[str, str], regions: Sequence[str],
                      service: str, malformed_code: str) -> tuple[str, str, str, str, str]:
    parts = cred.split("/")
    if len(parts) != 5 or parts[4] != "aws4_request":
        raise AuthFailure(400, malformed_code,
                          "The credential is mal-formed; expecting \"<YOUR-AKID>/YYYYMMDD/REGION/SERVICE/aws4_request\".")
    akid, date8, region, svc, _ = parts
    if akid not in keys:
        raise AuthFailure(403, "InvalidAccessKeyId",
                          "The AWS Access Key Id you provided does not exist in our records.",
                          AWSAccessKeyId=akid)
    if region not in regions:
        raise AuthFailure(400, malformed_code,
                          f"The authorization header is malformed; the region '{region}' is wrong; "
                          f"expecting '{regions[0]}'", Region=regions[0])
    if svc != service:
        raise AuthFailure(400, malformed_code,
                          f"The authorization header is malformed; incorrect service '{svc}'. "
                          f"This endpoint belongs to '{service}'.")
    return akid, date8, region, svc, keys[akid]


def verify(method: str, raw_path: str, query: Sequence[tuple[str, str]],
           headers: Sequence[tuple[str, str]], keys: Mapping[str, str], *, now: float,
           regions: Sequence[str] = ("auto",), service: str = "s3",
           body_sha256: str | None = None) -> AuthResult:
    """Verifies a request exactly like S3 does. Raises AuthFailure.

    ``raw_path`` is the decoded path; ``query`` the decoded (name, value) pairs in request order;
    ``headers`` the request headers; ``keys`` maps access key id -> secret. ``body_sha256`` (hex)
    enables the x-amz-content-sha256 payload check for header-signed requests with a body.
    """
    query = list(query)
    qnames = {k for k, _ in query}
    hmap = _header_lookup(headers)
    if "X-Amz-Algorithm" in qnames or "X-Amz-Signature" in qnames:
        return _verify_presigned(method, raw_path, query, hmap, keys, now, regions, service)
    auth = (hmap.get("authorization") or [""])[0]
    if not auth:
        raise AuthFailure(403, "AccessDenied", "Anonymous access is forbidden for this operation.")
    if not auth.startswith(ALGORITHM + " "):
        raise AuthFailure(400, "InvalidArgument",
                          "Unsupported Authorization Type (only AWS4-HMAC-SHA256 is accepted).")
    return _verify_header(method, raw_path, query, hmap, auth, keys, now, regions, service,
                          body_sha256)


def _verify_header(method, raw_path, query, hmap, auth, keys, now, regions, service, body_sha256):
    fields: dict[str, str] = {}
    for part in auth[len(ALGORITHM) + 1:].split(","):
        part = part.strip()
        if "=" not in part:
            raise AuthFailure(400, "AuthorizationHeaderMalformed",
                              "The authorization header is malformed.")
        k, v = part.split("=", 1)
        fields[k.strip()] = v.strip()
    for required in ("Credential", "SignedHeaders", "Signature"):
        if required not in fields:
            raise AuthFailure(400, "AuthorizationHeaderMalformed",
                              f"The authorization header is malformed; missing {required}.")
    akid, date8, region, svc, secret = _parse_credential(
        fields["Credential"], keys, regions, service, "AuthorizationHeaderMalformed")

    amz = (hmap.get("x-amz-date") or [""])[0]
    if not amz:
        raise AuthFailure(403, "AccessDenied",
                          "AWS authentication requires a valid Date or x-amz-date header")
    req_time = parse_amz_date(amz)
    if req_time is None:
        raise AuthFailure(403, "AccessDenied",
                          "AWS authentication requires a valid Date or x-amz-date header")
    if amz[:8] != date8:
        raise AuthFailure(403, "SignatureDoesNotMatch",
                          "The credential date does not match the x-amz-date header.")
    if abs(now - req_time) > MAX_CLOCK_SKEW:
        raise AuthFailure(403, "RequestTimeTooSkewed",
                          "The difference between the request time and the current time is too large.",
                          RequestTime=amz, ServerTime=amz_date(now))

    payload = (hmap.get("x-amz-content-sha256") or [""])[0]
    if not payload:
        raise AuthFailure(400, "InvalidRequest",
                          "Missing required header for this request: x-amz-content-sha256")
    if payload.startswith("STREAMING-"):
        raise AuthFailure(501, "NotImplemented",
                          "Streaming (chunked) SigV4 uploads are not supported by R2.")
    if payload != UNSIGNED_PAYLOAD and not _HEX64_RE.match(payload):
        raise AuthFailure(400, "InvalidArgument",
                          "x-amz-content-sha256 must be UNSIGNED-PAYLOAD or a lowercase hex SHA-256.")

    signed = [h for h in fields["SignedHeaders"].split(";") if h]
    if signed != sorted(signed) or len(set(signed)) != len(signed):
        raise AuthFailure(403, "SignatureDoesNotMatch", "SignedHeaders must be sorted and unique.")
    must_sign = {"host", "x-amz-date", "x-amz-content-sha256"}
    unsigned_amz = sorted(h for h in hmap if h.startswith("x-amz-") and h not in signed)
    if not must_sign.issubset(signed) or unsigned_amz:
        missing = sorted((must_sign - set(signed)) | set(unsigned_amz))
        raise AuthFailure(403, "AccessDenied",
                          "There were headers present in the request which were not signed",
                          HeadersNotSigned=", ".join(missing))
    sig_headers = []
    for name in signed:
        if name not in hmap:
            raise AuthFailure(403, "SignatureDoesNotMatch",
                              f"Signed header '{name}' is not present in the request.")
        for v in hmap[name]:
            sig_headers.append((name, v))
    block, signed_list = canonical_headers(sig_headers)
    creq = canonical_request(method, canonical_uri(raw_path), canonical_query(query), block,
                             signed_list, payload)
    scope = credential_scope(date8, region, svc)
    sts = string_to_sign(amz, scope, creq)
    expected = signature(signing_key(secret, date8, region, svc), sts)
    if not hmac.compare_digest(expected, fields["Signature"]):
        raise AuthFailure(403, "SignatureDoesNotMatch",
                          "The request signature we calculated does not match the signature you "
                          "provided. Check your secret access key and signing method.",
                          AWSAccessKeyId=akid, StringToSign=sts, CanonicalRequest=creq,
                          SignatureProvided=fields["Signature"])
    if body_sha256 is not None and payload != UNSIGNED_PAYLOAD and payload != body_sha256:
        raise AuthFailure(400, "XAmzContentSHA256Mismatch",
                          "The provided 'x-amz-content-sha256' header does not match what was computed.",
                          ClientComputedContentSHA256=payload, S3ComputedContentSHA256=body_sha256)
    return AuthResult("header", akid, payload, None, signed)


def _verify_presigned(method, raw_path, query, hmap, keys, now, regions, service):
    params: dict[str, str] = {}
    for k, v in query:
        params.setdefault(k, v)
    for required in ("X-Amz-Algorithm", "X-Amz-Credential", "X-Amz-Date", "X-Amz-Expires",
                     "X-Amz-SignedHeaders", "X-Amz-Signature"):
        if required not in params:
            raise AuthFailure(400, "AuthorizationQueryParametersError",
                              f"Query-string authentication version 4 requires the X-Amz-Algorithm, "
                              f"X-Amz-Credential, X-Amz-Signature, X-Amz-Date, X-Amz-SignedHeaders, "
                              f"and X-Amz-Expires parameters (missing {required}).")
    if params["X-Amz-Algorithm"] != ALGORITHM:
        raise AuthFailure(400, "AuthorizationQueryParametersError",
                          "X-Amz-Algorithm only supports \"AWS4-HMAC-SHA256\"")
    akid, date8, region, svc, secret = _parse_credential(
        params["X-Amz-Credential"], keys, regions, service, "AuthorizationQueryParametersError")
    try:
        expires = int(params["X-Amz-Expires"])
    except ValueError:
        expires = -1
    if not 1 <= expires <= MAX_PRESIGN_EXPIRES:
        raise AuthFailure(400, "AuthorizationQueryParametersError",
                          "X-Amz-Expires must be less than a week (in seconds) that is 604800")
    amz = params["X-Amz-Date"]
    req_time = parse_amz_date(amz)
    if req_time is None or amz[:8] != date8:
        raise AuthFailure(400, "AuthorizationQueryParametersError",
                          "X-Amz-Date must be in the ISO8601 Long Format \"yyyyMMdd'T'HHmmss'Z'\" "
                          "and match the credential date")
    signed = [h for h in params["X-Amz-SignedHeaders"].split(";") if h]
    if "host" not in signed:
        raise AuthFailure(400, "AuthorizationQueryParametersError",
                          "X-Amz-SignedHeaders must include host")
    sig_headers = []
    for name in signed:
        if name not in hmap:
            raise AuthFailure(403, "SignatureDoesNotMatch",
                              f"Signed header '{name}' is not present in the request.")
        for v in hmap[name]:
            sig_headers.append((name, v))
    payload = params.get("X-Amz-Content-Sha256", UNSIGNED_PAYLOAD)
    block, signed_list = canonical_headers(sig_headers)
    c_query = canonical_query([(k, v) for k, v in query if k != "X-Amz-Signature"])
    creq = canonical_request(method, canonical_uri(raw_path), c_query, block, signed_list, payload)
    scope = credential_scope(date8, region, svc)
    sts = string_to_sign(amz, scope, creq)
    expected = signature(signing_key(secret, date8, region, svc), sts)
    if not hmac.compare_digest(expected, params["X-Amz-Signature"]):
        raise AuthFailure(403, "SignatureDoesNotMatch",
                          "The request signature we calculated does not match the signature you "
                          "provided. Check your key and signing method.",
                          AWSAccessKeyId=akid, StringToSign=sts, CanonicalRequest=creq,
                          SignatureProvided=params["X-Amz-Signature"])
    # Expiry is checked after the signature (like S3: a tampered URL reports the signature).
    if now > req_time + expires:
        raise AuthFailure(403, "AccessDenied", "Request has expired",
                          XAmzExpires=str(expires), Expires=amz_date(req_time + expires),
                          ServerTime=amz_date(now))
    if req_time > now + MAX_CLOCK_SKEW:
        raise AuthFailure(403, "AccessDenied", "Request is not valid yet")
    return AuthResult("presigned", akid, payload, req_time + expires, signed)
