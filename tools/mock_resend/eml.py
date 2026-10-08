"""Raw RFC 5322 / MIME message building and header helpers (stdlib ``email`` package).

``build_eml`` produces what a receiving MTA would store for a message: multipart/alternative
text+html, inline images in multipart/related (``Content-ID``), regular attachments in
multipart/mixed, RFC 2047 encoded-words for non-ASCII headers (Chinese subjects and names) and
RFC 2231 ``filename*`` for non-ASCII filenames. Line endings are CRLF.
"""

from __future__ import annotations

import email.policy
import email.utils
import mimetypes
import re
import secrets
from dataclasses import dataclass
from email.header import Header, decode_header, make_header
from email.message import EmailMessage
from typing import Iterable, Sequence

_POLICY = email.policy.SMTP  # CRLF, encoded-words for non-ASCII


@dataclass
class Part:
    filename: str
    content_type: str
    data: bytes
    content_id: str | None = None  # without <>
    inline: bool = False


def guess_type(filename: str, fallback: str = "application/octet-stream") -> str:
    ctype, _ = mimetypes.guess_type(filename)
    return ctype or fallback


def new_message_id(domain: str = "mock.resend.local") -> str:
    return f"<{secrets.token_hex(12)}@{domain}>"


def angle(msgid: str) -> str:
    """Normalizes a Message-ID to carry angle brackets."""
    msgid = msgid.strip()
    if not msgid:
        return msgid
    if not msgid.startswith("<"):
        msgid = "<" + msgid
    if not msgid.endswith(">"):
        msgid = msgid + ">"
    return msgid


def _split_type(ctype: str) -> tuple[str, str]:
    main, _, sub = (ctype or "application/octet-stream").partition("/")
    main = main.strip().lower() or "application"
    sub = (sub.split(";")[0].strip().lower()) or "octet-stream"
    return main, sub


def build_eml(*, from_: str, to: Sequence[str], cc: Sequence[str] = (),
              reply_to: Sequence[str] = (), subject: str = "", html: str | None = None,
              text: str | None = None, message_id: str, date: float,
              headers: Iterable[tuple[str, str]] = (), attachments: Sequence[Part] = (),
              auth_results: str | None = None, subject_charset: str | None = None) -> bytes:
    """Builds the raw .eml. ``headers`` are extra headers (In-Reply-To, References, X-*)."""
    msg = EmailMessage(policy=_POLICY)
    if auth_results:
        msg["Authentication-Results"] = auth_results
    msg["From"] = from_
    if to:
        msg["To"] = ", ".join(to)
    if cc:
        msg["Cc"] = ", ".join(cc)
    if reply_to:
        msg["Reply-To"] = ", ".join(reply_to)
    msg["Subject"] = subject
    msg["Date"] = email.utils.formatdate(date, localtime=False, usegmt=True)
    msg["Message-ID"] = angle(message_id)
    msg["MIME-Version"] = "1.0"
    skip = {"from", "to", "cc", "bcc", "subject", "date", "message-id", "mime-version",
            "content-type", "content-transfer-encoding", "reply-to"}
    for name, value in headers:
        if name.lower() in skip:
            continue
        msg[name] = value

    inline = [a for a in attachments if a.inline and a.content_id]
    regular = [a for a in attachments if not (a.inline and a.content_id)]
    if html is not None and text is not None:
        msg.set_content(text)
        msg.add_alternative(html, subtype="html")
    elif html is not None:
        msg.set_content(html, subtype="html")
    else:
        msg.set_content(text or "")
    if inline:
        body = msg.get_body(("html",)) or msg.get_body(("plain",))
        assert body is not None
        for part in inline:
            main, sub = _split_type(part.content_type)
            body.add_related(part.data, maintype=main, subtype=sub, cid=angle(part.content_id or ""),
                             filename=part.filename, disposition="inline")
    for part in regular:
        main, sub = _split_type(part.content_type)
        msg.add_attachment(part.data, maintype=main, subtype=sub, filename=part.filename)

    raw = msg.as_bytes(policy=_POLICY)
    if subject_charset and subject:
        encoded = Header(subject, subject_charset).encode(linesep="\r\n")
        raw = replace_header(raw, "Subject", encoded)
    return raw


def split_raw(raw: bytes) -> tuple[bytes, bytes]:
    """(header block without the blank line, body)."""
    idx = raw.find(b"\r\n\r\n")
    if idx < 0:
        idx2 = raw.find(b"\n\n")
        if idx2 < 0:
            return raw, b""
        return raw[:idx2], raw[idx2 + 2:]
    return raw[:idx], raw[idx + 4:]


def header_lines(raw: bytes) -> list[tuple[str, str]]:
    """Unfolded (name, raw value) pairs of the top-level header block, in order."""
    block, _ = split_raw(raw)
    out: list[tuple[str, str]] = []
    for line in re.split(rb"\r?\n", block):
        if not line:
            continue
        if line[:1] in (b" ", b"\t") and out:
            name, value = out[-1]
            out[-1] = (name, value + " " + line.strip().decode("utf-8", "replace"))
            continue
        name, sep, value = line.partition(b":")
        if not sep:
            continue
        out.append((name.decode("ascii", "replace").strip(), value.strip().decode("utf-8", "replace")))
    return out


def replace_header(raw: bytes, name: str, new_value: str) -> bytes:
    """Replaces the (possibly folded) top-level header ``name`` with ``name: new_value``."""
    block, body = split_raw(raw)
    lines = re.split(rb"\r?\n", block)
    out: list[bytes] = []
    skipping = False
    replaced = False
    for line in lines:
        if skipping and line[:1] in (b" ", b"\t"):
            continue
        skipping = False
        if line.lower().startswith(name.lower().encode() + b":") and not replaced:
            out.append(f"{name}: {new_value}".encode("ascii"))
            skipping = True
            replaced = True
            continue
        out.append(line)
    return b"\r\n".join(out) + b"\r\n\r\n" + body


def headers_map(raw: bytes) -> dict[str, str]:
    """Lowercased header name -> unfolded raw value (first occurrence wins), like Resend's map."""
    out: dict[str, str] = {}
    for name, value in header_lines(raw):
        out.setdefault(name.lower(), value)
    return out


MX_HOST = "inbound-smtp.us-east-1.amazonaws.com"  # Resend's receiving MX (GET /domains)
_FOR_RE = re.compile(r"\bfor\s+<?([^\s<>;]+@[^\s<>;]+?)>?\s*(?:;|$)", re.IGNORECASE)


def received_header(*, relay: str, relay_ip: str, smtp_id: str, date: float,
                    for_rcpt: str | None) -> str:
    """Value of the Received trace header the receiving MX prepends (RFC 5321 §4.4), folded.

    ``for_rcpt`` is the single address of the FOR clause, or None for no clause (what Postfix,
    Exim and Sendmail write when the SMTP transaction had several RCPT TO).
    """
    fold = "\r\n        "
    value = f"from {relay} ({relay} [{relay_ip}]){fold}by {MX_HOST}{fold}with SMTP id {smtp_id}"
    if for_rcpt:
        value += f"{fold}for <{for_rcpt}>"
    return value + ";" + fold + email.utils.formatdate(date, localtime=False, usegmt=True)


def prepend_header(raw: bytes, name: str, value: str) -> bytes:
    """Adds a top-level header line in front (trace headers are prepended by each hop)."""
    return f"{name}: {value}\r\n".encode("utf-8") + raw


def received_for_clauses(raw: bytes) -> list[str]:
    """Addresses named by the FOR clauses of all Received headers, in order, deduplicated.

    This is how Resend documents ``received_for`` ("recipient addresses from the Received
    headers' for clause").
    """
    out: list[str] = []
    for name, value in header_lines(raw):
        if name.lower() != "received":
            continue
        m = _FOR_RE.search(value)
        if m:
            addr = m.group(1).strip().lower()
            if addr not in out:
                out.append(addr)
    return out


def decode_words(value: str) -> str:
    """Decodes RFC 2047 encoded-words (unknown charsets are kept as-is)."""
    try:
        return str(make_header(decode_header(value)))
    except (LookupError, UnicodeDecodeError, ValueError):
        return value


def auth_results_header(spf: str, dkim: str, dmarc: str, from_domain: str) -> str:
    return (f"mx.mock-resend.local; spf={spf} smtp.mailfrom={from_domain}; "
            f"dkim={dkim} header.d={from_domain}; dmarc={dmarc} header.from={from_domain}")


def addr_of(value: str) -> str:
    """Bare lowercase address of ``Name <a@b>`` / ``a@b``."""
    _, addr = email.utils.parseaddr(value or "")
    return addr.strip().lower()


def domain_of(address: str) -> str:
    return address.rsplit("@", 1)[-1].lower() if "@" in address else ""


def local_part(address: str) -> str:
    return address.split("@", 1)[0].lower() if "@" in address else address.lower()
