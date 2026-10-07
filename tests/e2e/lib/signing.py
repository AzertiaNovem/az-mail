"""Python replica of backend/src/core/signed_url.{hpp,cpp} (WP0, frozen).

key = HMAC-SHA256(server_secret, "azmail/signed-url/v1")
file: sig = b64url(HMAC(key, "file|<id>|<uid>|<exp>|<d>")), raw: "raw|<id>|<uid>|<exp>"

Used to forge *validly signed* URLs in tests (expired URL, another user's id) so the backend's
expiry and ownership checks are exercised independently of the signature check. Because the
config loader may decode AZMAIL_SECRET (hex/base64), ``detect`` picks the interpretation that
reproduces a URL the backend actually produced.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import urllib.parse


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


class SignedUrls:
    def __init__(self, secret: bytes, base: str) -> None:
        self.key = hmac.new(secret, b"azmail/signed-url/v1", hashlib.sha256).digest()
        self.base = base.rstrip("/")

    def sign_file(self, att_id: int, uid: int, d: str, exp: int) -> str:
        return _b64url(hmac.new(self.key, f"file|{att_id}|{uid}|{exp}|{d}".encode(), hashlib.sha256).digest())

    def sign_raw(self, msg_id: int, uid: int, exp: int) -> str:
        return _b64url(hmac.new(self.key, f"raw|{msg_id}|{uid}|{exp}".encode(), hashlib.sha256).digest())

    def file_url(self, att_id: int, uid: int, d: str, exp: int) -> str:
        return (f"{self.base}/api/files/{att_id}?d={d}&u={uid}&exp={exp}"
                f"&sig={self.sign_file(att_id, uid, d, exp)}")

    def raw_url(self, msg_id: int, uid: int, exp: int) -> str:
        return f"{self.base}/api/files/raw/{msg_id}?u={uid}&exp={exp}&sig={self.sign_raw(msg_id, uid, exp)}"

    def reproduces(self, url: str) -> bool:
        parts = urllib.parse.urlsplit(url)
        q = dict(urllib.parse.parse_qsl(parts.query))
        segs = [s for s in parts.path.split("/") if s]
        try:
            if segs[:3] == ["api", "files", "raw"]:
                return hmac.compare_digest(self.sign_raw(int(segs[3]), int(q["u"]), int(q["exp"])), q["sig"])
            if segs[:2] == ["api", "files"]:
                return hmac.compare_digest(self.sign_file(int(segs[2]), int(q["u"]), q["d"], int(q["exp"])),
                                           q["sig"])
        except (KeyError, ValueError, IndexError):
            return False
        return False


def candidates(secret: str) -> list[bytes]:
    out = [secret.encode()]
    try:
        if len(secret) % 2 == 0:
            out.append(bytes.fromhex(secret))
    except ValueError:
        pass
    for decoder in (base64.b64decode, base64.urlsafe_b64decode):
        try:
            out.append(decoder(secret + "=" * (-len(secret) % 4)))
        except (binascii.Error, ValueError):
            pass
    unique: list[bytes] = []
    for c in out:
        if c not in unique:
            unique.append(c)
    return unique


def detect(secret: str, base: str, observed_url: str) -> SignedUrls | None:
    """The signer that reproduces ``observed_url``, or None (secret interpreted differently)."""
    for cand in candidates(secret):
        signer = SignedUrls(cand, base)
        if signer.reproduces(observed_url):
            return signer
    return None
