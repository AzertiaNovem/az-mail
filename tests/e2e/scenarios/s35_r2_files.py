"""E2E 35 (Addendum A) — R2 file delivery: presigned redirect works; tampered / expired presign →
403; proxy mode streams through the API origin. Every backend → R2 request is SigV4-verified by
the mock exactly like S3."""

from __future__ import annotations

import time
import urllib.parse

from lib import flows
from lib.env import R2_BUCKET, Ctx, Skip

TITLE = "R2: presigned redirect + overrides, tampered/expired → 403, proxy streaming"
TIMEOUT = 150

PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")
HTML = b"<html><script>alert(document.domain)</script></html>"


def run(ctx: Ctx) -> None:
    env = ctx.env
    if env.blob_backend != "r2":
        raise Skip("R2-only scenario (blob backend is local)")
    alice = ctx.team().alice
    s3_since = ctx.mark()

    ctx.restart_backend(AZMAIL_FILES_DELIVERY="redirect", R2_PRESIGN_TTL_SEC="3")
    me = alice.api.get("/api/auth/me")
    assert env.s3_base in me["server"]["files_origins"], me["server"]
    assert env.api_base in me["server"]["files_origins"], me["server"]
    storage = ctx.admin.get("/api/admin/stats")["storage"]
    assert storage["backend"] == "r2" and storage["delivery"] == "redirect", storage

    png = flows.upload(alice.api, "照片.png", "image/png", PNG, inline=True)
    page = flows.upload(alice.api, "page.html", "text/html", HTML)

    r = flows.fetch(png["download_url"])
    assert r.status == 302, (r.status, r.short())
    loc = r.header("location") or ""
    assert loc.startswith(f"{env.s3_base}/{R2_BUCKET}/"), loc[:80]
    assert "X-Amz-Signature=" in loc and "X-Amz-Expires=" in loc, "not a presigned URL"
    cache = (r.header("cache-control") or "").replace(" ", "")
    assert "private" in cache and "max-age=60" in cache, r.headers

    s3 = flows.fetch(loc)
    assert s3.status == 200 and s3.body == PNG, (s3.status, s3.short())
    assert (s3.header("content-type") or "").startswith("image/png"), s3.headers
    disp = s3.header("content-disposition") or ""
    assert disp.startswith("attachment") and urllib.parse.quote("照片.png", safe="").lower() in disp.lower(), disp

    view = flows.fetch(png["view_url"])
    assert view.status == 302, view.status
    v = flows.fetch(view.header("location") or "")
    assert v.status == 200 and (v.header("content-disposition") or "").startswith("inline"), v.headers

    r = flows.fetch(page["download_url"])
    assert r.status == 302, r.status
    unsafe = flows.fetch(r.header("location") or "")
    assert unsafe.status == 200 and unsafe.body == HTML, unsafe.status
    assert unsafe.header("content-type") == "application/octet-stream", \
        f"unsafe types must be forced to octet-stream on R2: {unsafe.headers}"
    assert (unsafe.header("content-disposition") or "").startswith("attachment"), unsafe.headers

    sig = flows.query_of(loc)["X-Amz-Signature"]
    flipped = loc.replace(sig, ("0" if sig[0] != "0" else "1") + sig[1:])
    r = flows.fetch(flipped)
    assert r.status == 403 and b"SignatureDoesNotMatch" in r.body, (r.status, r.short())
    swapped = loc.replace("response-content-type=image%2Fpng", "response-content-type=text%2Fhtml")
    assert swapped != loc, "presigned URL lacks the response-content-type override"
    r = flows.fetch(swapped)
    assert r.status == 403 and b"SignatureDoesNotMatch" in r.body, (r.status, r.short())

    time.sleep(4.5)  # R2_PRESIGN_TTL_SEC=3
    r = flows.fetch(loc)
    assert r.status == 403 and b"Request has expired" in r.body, (r.status, r.short())
    r = flows.fetch(png["download_url"])  # our own signed URL is still valid → a fresh presign
    assert r.status == 302 and flows.fetch(r.header("location") or "").status == 200

    failures = [q for q in ctx.mock.s3(s3_since)["requests"]
                if q["auth"] == "header" and q.get("auth_ok") is False]
    assert not failures, f"backend → R2 requests failed SigV4 verification: {failures[:3]}"
    assert all(q["auth"] != "none" for q in ctx.mock.s3(s3_since)["requests"]), "unauthenticated R2 request"

    ctx.restart_backend()  # default: proxy
    r = flows.fetch(png["download_url"])
    assert r.status == 200 and r.body == PNG, (r.status, r.short())
    assert r.header("location") is None
    assert (r.header("content-type") or "").startswith("image/png"), r.headers
    assert r.header("x-content-type-options") == "nosniff", r.headers
    r = flows.fetch(page["download_url"])
    assert r.status == 200 and "sandbox" in (r.header("content-security-policy") or ""), r.headers
    me = alice.api.get("/api/auth/me")
    assert env.s3_base not in me["server"]["files_origins"], me["server"]
    assert ctx.admin.get("/api/admin/stats")["storage"]["delivery"] == "proxy"
