#!/usr/bin/env python3
"""Manual probe of real Resend behaviour that AZ Mail depends on (DESIGN §1 F4).

Needs a REAL full-access API key and a test domain that is verified for sending AND has receiving
enabled (MX → inbound-smtp.us-east-1.amazonaws.com). It sends a handful of emails to addresses
on that domain only (plus --external if given) and prints its findings. Python stdlib only.

    export RESEND_API_KEY=re_...
    python3 tools/resend_probe.py --domain probe.example.com            # dry run: shows the plan
    python3 tools/resend_probe.py --domain probe.example.com --yes --json probe-report.json

Checks (F4): 1 received_for + split delivery (to + cc + bcc on the domain); 2 whether the
received `headers` map contains In-Reply-To / References / X-*; 3 list order and after/before
cursor semantics; 4 scheduled send with attachments; 5 whether a custom Message-ID header is
honoured; 6 when message_id becomes available (immediate and scheduled sends); 7 whether
received `from` / `subject` are already decoded; 8 which key permission the receiving endpoints
need (--sending-key: a sending-only key to compare); 9 whether an empty subject is accepted.
Extras: Cloudflare 1010 without User-Agent, idempotency replay/conflict, rate-limit headers,
html_format=cid, raw download, attachment download_url + Authorization header behaviour.
"""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Any

API = "https://api.resend.com"
UA = "azmail-resend-probe/1.0"
PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")


class Probe:
    def __init__(self, key: str, base: str, min_interval: float = 0.3) -> None:
        self.key = key
        self.base = base.rstrip("/")
        self.min_interval = min_interval
        self._last = 0.0
        self.findings: dict[str, Any] = {}

    # -- HTTP -------------------------------------------------------------------------------------
    def call(self, method: str, path: str, body: Any = None, *, key: str | None = None,
             headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], Any]:
        wait = self.min_interval - (time.monotonic() - self._last)
        if wait > 0:
            time.sleep(wait)  # stay well below the shared 10 req/s team limit
        self._last = time.monotonic()
        hdrs = {"Authorization": f"Bearer {key or self.key}", "User-Agent": UA}
        data = None
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            hdrs["Content-Type"] = "application/json"
        hdrs.update(headers or {})
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                status, rh, raw = resp.status, dict(resp.headers.items()), resp.read()
        except urllib.error.HTTPError as err:
            status, rh, raw = err.code, dict(err.headers.items()), err.read()
            err.close()
        try:
            parsed: Any = json.loads(raw) if raw else None
        except ValueError:
            parsed = raw.decode("utf-8", "replace")
        return status, {k.lower(): v for k, v in rh.items()}, parsed

    def send(self, body: dict[str, Any], idem: str | None = None) -> tuple[int, Any]:
        headers = {"Idempotency-Key": idem} if idem else None
        status, _, out = self.call("POST", "/emails", body, headers=headers)
        return status, out

    def received_with(self, token: str, timeout: float) -> list[dict[str, Any]]:
        """Polls the receiving list until mails whose subject contains ``token`` stop arriving."""
        deadline = time.monotonic() + timeout
        found: dict[str, dict[str, Any]] = {}
        stable_since = None
        while time.monotonic() < deadline:
            status, _, page = self.call("GET", "/emails/receiving?limit=100")
            if status != 200:
                raise SystemExit(f"GET /emails/receiving → {status}: {page}")
            for item in page.get("data", []):
                if token in (item.get("subject") or ""):
                    found.setdefault(item["id"], item)
            if found:
                stable_since = stable_since or time.monotonic()
                if time.monotonic() - stable_since > 20:  # wait for split copies
                    break
            time.sleep(3)
        return list(found.values())

    def record(self, check: str, **facts: Any) -> None:
        self.findings[check] = facts
        print(f"\n[{check}]")
        for k, v in facts.items():
            text = json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v
            print(f"  {k}: {text}")


def iso_in(seconds: int) -> str:
    return (datetime.now(timezone.utc) + timedelta(seconds=seconds)).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def no_user_agent(base: str, key: str) -> tuple[int, str]:
    parts = urllib.parse.urlsplit(base)
    if parts.scheme == "http":  # the mock (tools/mock_resend) for a rehearsal run
        conn: http.client.HTTPConnection = http.client.HTTPConnection(parts.hostname, parts.port or 80, timeout=20)
    else:
        conn = http.client.HTTPSConnection(parts.hostname, parts.port or 443, timeout=20)
    conn.putrequest("GET", "/domains", skip_accept_encoding=True)
    conn.putheader("Authorization", f"Bearer {key}")
    conn.endheaders()
    resp = conn.getresponse()
    out = resp.status, resp.read()[:200].decode("utf-8", "replace")
    conn.close()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--domain", required=True, help="verified test domain with receiving enabled")
    ap.add_argument("--external", default="", help="optional external mailbox (delivered events)")
    ap.add_argument("--sending-key", default=os.environ.get("RESEND_SENDING_KEY", ""),
                    help="optional sending-only key to compare receiving permissions (check 8)")
    ap.add_argument("--api-base", default=os.environ.get("RESEND_API_BASE", API))
    ap.add_argument("--timeout", type=float, default=240, help="seconds to wait for inbound mail")
    ap.add_argument("--json", default="", help="write the findings here")
    ap.add_argument("--yes", action="store_true", help="really send (otherwise a dry run)")
    args = ap.parse_args()

    key = os.environ.get("RESEND_API_KEY", "")
    d = args.domain.lower()
    token = "p" + secrets.token_hex(4)
    to_a, to_b, bcc_c = f"probe-a@{d}", f"probe-b@{d}", f"probe-c@{d}"
    sender = f"probe@{d}"
    plan = [
        f"1+2+6+7  {sender} → to {to_a}, cc {to_b}, bcc {bcc_c}  (Chinese subject, custom headers)",
        f"4        scheduled (+10 min) with an attachment → {to_a}  (canceled right away if accepted)",
        f"5        custom Message-ID header → {to_a}",
        f"6        scheduled (+10 min) without attachment → {to_a}  (canceled)",
        f"9        empty subject → {to_a}",
        "extras   idempotency replay/conflict (one more email → " + (args.external or to_a) + ")",
    ]
    print(f"Resend probe — token {token}, domain {d}\nPlanned emails:\n  " + "\n  ".join(plan))
    if not args.yes:
        print("\nDry run. Re-run with --yes to send (requires RESEND_API_KEY, full access).")
        return 0
    if not key.startswith("re_"):
        print("RESEND_API_KEY is not set (or not a re_… key)", file=sys.stderr)
        return 2
    p = Probe(key, args.api_base)

    # extras: edge behaviour --------------------------------------------------------------------
    status, body = no_user_agent(args.api_base, key)
    st, hdrs, _ = p.call("GET", "/domains")
    p.record("extra.edge", no_user_agent_status=status, no_user_agent_body=body,
             domains_status=st, ratelimit_headers={k: v for k, v in hdrs.items() if "ratelimit" in k or k == "retry-after"})

    # 8: receiving permission ------------------------------------------------------------------------
    st_full, _, out_full = p.call("GET", "/emails/receiving?limit=1")
    facts: dict[str, Any] = {"full_key_status": st_full}
    if args.sending_key:
        st_s, _, out_s = p.call("GET", "/emails/receiving?limit=1", key=args.sending_key)
        facts.update(sending_key_status=st_s, sending_key_error=out_s)
    else:
        facts["note"] = "pass --sending-key to compare a sending-only key"
    if st_full != 200:
        facts["full_key_error"] = out_full
    p.record("F4.8 receiving permission", **facts)

    # 1, 2, 6, 7: one email to two inboxes + bcc -------------------------------------------------------
    subject = f"探针 主题 ✓ {token}"
    parent = f"<parent-{token}@{d}>"
    t0 = time.monotonic()
    st, out = p.send({
        "from": f"探针 测试 <{sender}>", "to": [to_a], "cc": [to_b], "bcc": [bcc_c], "subject": subject,
        "html": f"<p>probe {token} <img src=\"cid:probe-img\"></p>", "text": f"probe {token}",
        "headers": {"In-Reply-To": parent, "References": f"<root-{token}@{d}> {parent}",
                    "X-AzMail-Ref": token},
        "tags": [{"name": "azmail_outbound", "value": token}],
        "attachments": [{"filename": "探针.png", "content": base64.b64encode(PNG).decode(),
                         "content_type": "image/png", "content_id": "probe-img"}],
    }, idem=f"probe-{token}-main")
    if st != 200:
        print(f"send failed: {st} {out}", file=sys.stderr)
        return 1
    main_id = out["id"]
    mid_delay = None
    while time.monotonic() - t0 < 90:
        _, _, got = p.call("GET", f"/emails/{main_id}")
        if got.get("message_id"):
            mid_delay = round(time.monotonic() - t0, 1)
            break
        time.sleep(1)
    _, _, sent = p.call("GET", f"/emails/{main_id}")
    p.record("F4.6 message_id availability (immediate send)", seconds_until_message_id=mid_delay,
             message_id=sent.get("message_id"), created_at_format=sent.get("created_at"),
             last_event=sent.get("last_event"), tags_shape=sent.get("tags"))

    received = p.received_with(token, args.timeout)
    copies = [r for r in received if r.get("subject", "").endswith(token) and "主题" in r.get("subject", "")]
    details = []
    for r in copies:
        _, _, full = p.call("GET", f"/emails/receiving/{r['id']}?html_format=cid")
        details.append(full)
    p.record("F4.1 received_for / split delivery",
             receiving_emails=len(copies),
             received_for=[dd.get("received_for") for dd in details],
             # Resend documents received_for as the Received headers' `for` clauses: compare
             # (mock: tools/mock_resend/README.md "Envelope", received_for_mode)
             received_headers=[(dd.get("headers") or {}).get("received") for dd in details],
             to=[dd.get("to") for dd in details], cc=[dd.get("cc") for dd in details],
             bcc=[dd.get("bcc") for dd in details],
             message_ids=[dd.get("message_id") for dd in details],
             sent_message_id=sent.get("message_id"))
    if details:
        dd = details[0]
        hkeys = sorted((dd.get("headers") or {}).keys())
        p.record("F4.2 headers map",
                 keys=hkeys,
                 has_in_reply_to=any(k.lower() == "in-reply-to" for k in hkeys),
                 has_references=any(k.lower() == "references" for k in hkeys),
                 has_x_azmail_ref=any(k.lower() == "x-azmail-ref" for k in hkeys),
                 names_lowercase=all(k == k.lower() for k in hkeys))
        p.record("F4.7 decoded from/subject",
                 subject=dd.get("subject"), subject_matches=dd.get("subject") == subject,
                 from_=dd.get("from"), header_subject=(dd.get("headers") or {}).get("subject"),
                 header_from=(dd.get("headers") or {}).get("from"))
        raw = dd.get("raw") or {}
        atts_status, _, atts = p.call("GET", f"/emails/receiving/{dd['id']}/attachments")
        att = (atts.get("data") or [{}])[0] if atts_status == 200 else {}
        dl = {}
        if att.get("download_url"):
            for with_auth in (False, True):
                req = urllib.request.Request(att["download_url"], headers={"User-Agent": UA, **(
                    {"Authorization": f"Bearer {key}"} if with_auth else {})})
                try:
                    with urllib.request.urlopen(req, timeout=30) as resp:
                        dl["with_auth" if with_auth else "plain"] = [resp.status, len(resp.read())]
                except urllib.error.HTTPError as err:
                    dl["with_auth" if with_auth else "plain"] = [err.code, err.read()[:120].decode("utf-8", "replace")]
                    err.close()
        p.record("extra.receiving details", html_format=dd.get("html_format"),
                 html_has_cid="cid:" in (dd.get("html") or ""), authentication=dd.get("authentication"),
                 raw_present=bool(raw), raw_expires_at=raw.get("expires_at"),
                 attachment=att and {k: att.get(k) for k in ("filename", "content_type", "content_id",
                                                             "content_disposition", "size", "expires_at")},
                 download_url_host=urllib.parse.urlsplit(att.get("download_url", "")).hostname,
                 download_results=dl)
    else:
        p.record("F4.2 headers map", error=f"no inbound copy within {args.timeout:.0f}s")

    # 3: list order and cursors ----------------------------------------------------------------------
    _, _, page = p.call("GET", "/emails/receiving?limit=3")
    data = page.get("data") or []
    order = [x.get("created_at") for x in data]
    facts = {"first_page_created_at": order, "has_more": page.get("has_more"),
             "newest_first": order == sorted(order, reverse=True)}
    if data:
        _, _, after = p.call("GET", f"/emails/receiving?limit=3&after={data[-1]['id']}")
        _, _, before = p.call("GET", f"/emails/receiving?limit=3&before={data[0]['id']}")
        facts.update(after_created_at=[x.get("created_at") for x in after.get("data") or []],
                     before_created_at=[x.get("created_at") for x in before.get("data") or []])
    p.record("F4.3 list order / cursors", **facts)

    # 4: scheduled with attachment --------------------------------------------------------------------
    st, out = p.send({"from": sender, "to": [to_a], "subject": f"scheduled+attachment {token}",
                      "text": "probe", "scheduled_at": iso_in(600),
                      "attachments": [{"filename": "a.txt", "content": base64.b64encode(b"probe").decode()}]})
    facts = {"status": st, "response": out}
    if st == 200:
        _, _, got = p.call("GET", f"/emails/{out['id']}")
        cst, _, cout = p.call("POST", f"/emails/{out['id']}/cancel")
        facts.update(last_event=got.get("last_event"), cancel_status=cst, cancel_response=cout)
    p.record("F4.4 scheduled send with attachments", **facts)

    # 6b: message_id while scheduled ------------------------------------------------------------------
    st, out = p.send({"from": sender, "to": [to_a], "subject": f"scheduled {token}", "text": "probe",
                      "scheduled_at": iso_in(600)})
    facts = {"status": st}
    if st == 200:
        _, _, got = p.call("GET", f"/emails/{out['id']}")
        pst, _, pout = p.call("PATCH", f"/emails/{out['id']}", {"scheduled_at": iso_in(1200)})
        cst, _, _ = p.call("POST", f"/emails/{out['id']}/cancel")
        c2, _, c2out = p.call("POST", f"/emails/{out['id']}/cancel")
        facts.update(message_id_while_scheduled=got.get("message_id"), scheduled_at=got.get("scheduled_at"),
                     last_event=got.get("last_event"), patch_status=pst, patch_response=pout,
                     cancel_status=cst, second_cancel=[c2, c2out])
    p.record("F4.6b message_id while scheduled", **facts)

    # 5: custom Message-ID ---------------------------------------------------------------------------
    custom = f"<custom-{token}@{d}>"
    st, out = p.send({"from": sender, "to": [to_a], "subject": f"custom message-id {token}", "text": "probe",
                      "headers": {"Message-ID": custom}})
    facts = {"status": st, "requested": custom}
    if st == 200:
        time.sleep(10)
        _, _, got = p.call("GET", f"/emails/{out['id']}")
        facts.update(message_id=got.get("message_id"), honoured=(got.get("message_id") or "").strip("<>") == custom.strip("<>"))
    p.record("F4.5 custom Message-ID", **facts)

    # 9: empty subject ---------------------------------------------------------------------------------
    st, out = p.send({"from": sender, "to": [to_a], "subject": "", "text": f"empty subject {token}"})
    p.record("F4.9 empty subject", status=st, response=out)

    # extras: idempotency ------------------------------------------------------------------------------
    idem = f"probe-{token}-idem"
    body = {"from": sender, "to": [args.external or to_a], "subject": f"idempotency {token}", "text": "probe"}
    s1, o1 = p.send(body, idem)
    s2, o2 = p.send(body, idem)
    s3, o3 = p.send(dict(body, text="different"), idem)
    p.record("extra.idempotency", first=[s1, o1], replay=[s2, o2], same_id=o1 == o2,
             different_body=[s3, o3])

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"token": token, "domain": d, "findings": p.findings}, fh, ensure_ascii=False, indent=2)
        print(f"\nfindings written to {args.json}")
    print("\nCompare with the assumptions in docs/DESIGN.md §1 (F4) and tools/mock_resend/README.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
