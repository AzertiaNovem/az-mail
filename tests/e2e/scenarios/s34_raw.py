"""E2E 34 — raw .eml ("show original"): available for inbound mail and byte-identical."""

from __future__ import annotations

from lib import flows
from lib.env import EXTERNAL, Ctx

TITLE = "show original: raw .eml via Bearer and signed URL, byte-identical; none for outbound"
SMOKE = True


def run(ctx: Ctx) -> None:
    alice = ctx.team().alice
    tok = ctx.uniq("S34")
    subject = f"原始邮件 {tok}"
    inj = ctx.mock.inbound(from_=f"原件发送人 <raw.{tok.lower()}@{EXTERNAL}>", to=[alice.email], subject=subject,
                           html="<p>原文内容</p>", text="原文内容", headers={"X-Trace-Token": tok},
                           attachments=[{"filename": "说明.txt", "content_type": "text/plain",
                                         "data": "附件内容".encode()}])
    raw = ctx.mock.received_raw(inj["ids"][0])
    msg = flows.inbox_copy(alice, subject)
    assert msg["raw_url"], msg

    r = alice.api.request("GET", f"/api/messages/{msg['id']}/raw", expect=200)
    assert (r.header("content-type") or "").startswith("text/plain"), r.headers
    assert r.body == raw, f"raw differs ({len(r.body)} vs {len(raw)} bytes)"
    assert f"X-Trace-Token: {tok}".encode() in r.body

    r = flows.fetch(msg["raw_url"])
    assert r.status == 200 and r.body == raw, (r.status, len(r.body))
    disp = r.header("content-disposition") or ""
    assert disp.startswith("attachment") and ".eml" in disp, disp
    sig = flows.query_of(msg["raw_url"])["sig"]
    bad = msg["raw_url"].replace("sig=" + sig, "sig=" + ("A" if sig[0] != "A" else "B") + sig[1:])
    r = flows.fetch(bad)
    assert r.status == 403 and r.error_code == "invalid_signature", r.short()

    out = flows.send(alice.api, to=[f"ok.{tok.lower()}@{EXTERNAL}"], subject=f"出站无原件 {tok}")
    om = flows.message(alice.api, out["message_id"])
    assert om["raw_url"] is None, om["raw_url"]
    r = alice.api.call("GET", f"/api/messages/{out['message_id']}/raw")
    assert r.status == 404 and r.error_code == "not_found", r.short()
