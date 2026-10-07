"""E2E 14 — attachments: Chinese filename + inline image (+ SVG).

Mock gets content_id; outgoing html has cid: (no signed URLs, no data-att-id); the recipient's
backend downloads via the /_dl → /_blob redirect without credentials; signed URLs work,
tampered/expired → 403; RFC 6266 filename*; SVG is always served as an attachment with a
sandbox CSP and nosniff (DESIGN D4/D5).
"""

from __future__ import annotations

import hashlib
import os
import time
import urllib.parse

from lib import flows
from lib.env import Ctx

TITLE = "attachments: Chinese filename, inline cid image, downloads, signed URLs, SVG safety"
SMOKE = True
TIMEOUT = 150

PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")
SVG = b'<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _tamper(url: str) -> str:
    q = flows.query_of(url)
    sig = q["sig"]
    bad = ("A" if sig[0] != "A" else "B") + sig[1:]
    return url.replace("sig=" + sig, "sig=" + bad)


def run(ctx: Ctx) -> None:
    team = ctx.team()
    alice, bob = team.alice, team.bob
    pdf_name = "季度报告 2026.pdf"
    pdf = b"%PDF-1.4\n" + os.urandom(4096)
    a_pdf = flows.upload(alice.api, pdf_name, "application/pdf", pdf)
    assert a_pdf["filename"] == pdf_name and a_pdf["size"] == len(pdf), a_pdf
    assert a_pdf["inline"] is False and a_pdf["content_type"] == "application/pdf", a_pdf
    a_png = flows.upload(alice.api, "logo.png", "image/png", PNG, inline=True)
    assert a_png["inline"] is True and a_png["content_id"], a_png
    assert a_png["view_url"], a_png
    a_svg = flows.upload(alice.api, "icon.svg", "image/svg+xml", SVG)

    subject = f"附件测试 {ctx.uniq('S14')}"
    html = (f'<p>请查收附件</p><p><img data-att-id="{a_png["id"]}" src="{a_png["view_url"]}" '
            f'alt="logo"></p>')
    since = ctx.mark()
    res = flows.send(alice.api, to=[bob], subject=subject, html=html,
                     attachment_ids=[a_pdf["id"], a_png["id"], a_svg["id"]])

    email = flows.wait_mock_email(ctx, subject, since)[0]
    atts = {a["filename"]: a for a in email["attachments"]}
    assert set(atts) == {pdf_name, "logo.png", "icon.svg"}, list(atts)
    assert atts["logo.png"]["content_id"] == a_png["content_id"], atts["logo.png"]
    assert atts["logo.png"]["sha256"] == _sha(PNG) and atts[pdf_name]["sha256"] == _sha(pdf)
    assert atts[pdf_name]["content_id"] is None, atts[pdf_name]
    assert f"cid:{a_png['content_id']}" in email["html"], email["html"]
    assert "/api/files/" not in email["html"] and "data-att-id" not in email["html"], email["html"]

    copy = flows.inbox_copy(bob, subject, timeout=40)
    b = {a["filename"]: a for a in copy["attachments"]}
    assert set(b) == {pdf_name, "logo.png", "icon.svg"}, list(b)
    assert b["logo.png"]["inline"] is True and b["logo.png"]["content_id"] == a_png["content_id"], b["logo.png"]
    assert "cid:" not in (copy["html"] or ""), "read-time html must use signed URLs"
    assert "/api/files/" in (copy["html"] or ""), copy["html"]
    assert ctx.mock.violations() == [], f"credentials sent to download URLs: {ctx.mock.violations()}"

    r = flows.fetch(b[pdf_name]["download_url"])
    assert r.status == 200 and r.body == pdf, (r.status, r.short(120))
    disp = r.header("content-disposition") or ""
    assert disp.startswith("attachment"), disp
    assert ("filename*=utf-8''" + urllib.parse.quote(pdf_name, safe="")).lower() in disp.lower(), disp
    assert r.header("x-content-type-options") == "nosniff", r.headers

    r = flows.fetch(b["logo.png"]["view_url"])
    assert r.status == 200 and r.body == PNG, r.status
    assert (r.header("content-type") or "").startswith("image/png"), r.headers
    assert (r.header("content-disposition") or "").startswith("inline"), r.headers

    r = flows.fetch(b["icon.svg"]["download_url"])
    assert r.status == 200 and r.body == SVG, r.status
    assert (r.header("content-disposition") or "").startswith("attachment"), r.headers
    assert "sandbox" in (r.header("content-security-policy") or ""), r.headers
    assert r.header("x-content-type-options") == "nosniff", r.headers
    if b["icon.svg"]["view_url"]:
        r = flows.fetch(b["icon.svg"]["view_url"])
        assert (r.header("content-disposition") or "").startswith("attachment"), "SVG must never be inline"

    r = flows.fetch(_tamper(b[pdf_name]["download_url"]))
    assert r.status == 403 and r.error_code == "invalid_signature", r.short()

    signer = ctx.signer(b[pdf_name]["download_url"])
    if signer is not None:
        expired = signer.file_url(b[pdf_name]["id"], bob.id, "a", flows.now_ms() - 1000)
        r = flows.fetch(expired)
        assert r.status == 403 and r.error_code == "invalid_signature", r.short()
        exp = int(flows.query_of(b["icon.svg"]["download_url"])["exp"])
        r = flows.fetch(signer.file_url(b["icon.svg"]["id"], bob.id, "i", exp))
        assert r.status == 200 and (r.header("content-disposition") or "").startswith("attachment"), \
            "SVG requested inline must still be an attachment"
    else:  # the loader interprets AZMAIL_SECRET differently: prove expiry with a short TTL
        ctx.log("cannot replicate URL signing; checking expiry with AZMAIL_SIGNED_URL_TTL_SEC=2")
        ctx.restart_backend(AZMAIL_SIGNED_URL_TTL_SEC="2")
        url = flows.message(bob.api, copy["id"])["attachments"][0]["download_url"]
        time.sleep(3)
        r = flows.fetch(url)
        assert r.status == 403 and r.error_code == "invalid_signature", r.short()

    sent = flows.message(alice.api, res["message_id"])
    assert {a["filename"] for a in sent["attachments"]} == {pdf_name, "logo.png", "icon.svg"}
    flows.wait_status(alice.api, res["message_id"], "delivered", 30)
