#!/usr/bin/env python3
"""Seeds a local AZ Mail dev instance with a demo team and Chinese demo mail.

Normally run by scripts/dev.sh on the first start. Standalone:

    python3 scripts/seed_dev.py --api http://127.0.0.1:8080 --mock http://127.0.0.1:8787 \\
        --azmail-bin backend/build/mac-debug/azmail --credentials data/dev/credentials.txt

It uses the azmail CLI (add-domain, create-user --admin with the password on stdin — the
backend's environment must be exported, as dev.sh does) and then the REST API:
  * domain azmail.test, admin admin@azmail.test (管理员);
  * users 张三 zhangsan@, 李四 lisi@, 王五 wangwu@ (one random demo password, printed once and
    saved to --credentials with mode 0600);
  * alias support@azmail.test (客服支持): 张三 may send as it, 李四 is a member;
  * labels, inbound mail injected through the mock (attachments, inline image, GB18030 subject,
    remote images, a DMARC-failing phishing mail, mail to the alias), API sends with replies,
    CC, attachments, a bounce, a draft and a scheduled send.
Python standard library only.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

PNG_1PX = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")
PDF_DEMO = (b"%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj 2 0 obj<</Type/Pages/Kids[3 0 R]"
            b"/Count 1>>endobj 3 0 obj<</Type/Page/MediaBox[0 0 200 100]/Parent 2 0 R>>endobj\n"
            b"trailer<</Root 1 0 R>>\n%%EOF\n")


class SeedError(RuntimeError):
    pass


class Client:
    def __init__(self, base: str, token: str | None = None) -> None:
        self.base = base.rstrip("/")
        self.token = token

    def call(self, method: str, path: str, body: Any = None, *, data: bytes | None = None,
             headers: dict[str, str] | None = None, expect: tuple[int, ...] = (200, 201, 202, 204)) -> Any:
        hdrs = {"User-Agent": "azmail-seed/1.0"}
        if self.token:
            hdrs["Authorization"] = f"Bearer {self.token}"
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            hdrs["Content-Type"] = "application/json"
        hdrs.update(headers or {})
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                status, raw = resp.status, resp.read()
        except urllib.error.HTTPError as err:
            status, raw = err.code, err.read()
            err.close()
        except urllib.error.URLError as err:
            raise SeedError(f"{method} {path}: {err.reason}") from None
        if status not in expect:
            raise SeedError(f"{method} {path} → HTTP {status}: {raw[:300].decode('utf-8', 'replace')}")
        return json.loads(raw) if raw else None


def login(api: str, email: str, password: str) -> Client:
    data = Client(api).call("POST", "/api/auth/login", {"email": email, "password": password}, expect=(200,))
    return Client(api, data["token"])


def addr(name: str, email: str) -> dict[str, str]:
    return {"name": name, "email": email}


def wait_thread(client: Client, subject: str, timeout: float = 30.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        page = client.call("GET", "/api/threads?folder=all&limit=100")
        for item in page["items"]:
            if item["subject"] == subject:
                return item
        time.sleep(0.5)
    raise SeedError(f"thread {subject!r} did not appear within {timeout:.0f}s")


def send(client: Client, *, to: list[dict], subject: str, html: str, cc: list[dict] | None = None,
         scheduled_at: int | None = None, **extra: Any) -> dict[str, Any]:
    draft = client.call("POST", "/api/drafts", {"to": to, "cc": cc or [], "bcc": [], "subject": subject,
                                                "html": html, **extra}, expect=(201,))
    body: dict[str, Any] = {"version": draft["version"]}
    if scheduled_at is not None:
        body["scheduled_at"] = scheduled_at
    return client.call("POST", f"/api/drafts/{draft['id']}/send", body, expect=(202,))


def upload(client: Client, filename: str, content_type: str, data: bytes, inline: bool = False) -> dict:
    q = urllib.parse.urlencode({"filename": filename, "inline": "1" if inline else "0"},
                               quote_via=urllib.parse.quote)
    return client.call("POST", f"/api/attachments?{q}", data=data,
                       headers={"Content-Type": content_type}, expect=(201,))


def inject(mock: str, **body: Any) -> dict[str, Any]:
    return Client(mock).call("POST", "/_mock/inbound", body, expect=(200,))


def run_cli(azmail: str, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run([azmail, *args], input=stdin, capture_output=True, text=True, timeout=120)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--api", default="http://127.0.0.1:8080")
    ap.add_argument("--mock", default="http://127.0.0.1:8787")
    ap.add_argument("--azmail-bin", required=True)
    ap.add_argument("--domain", default="azmail.test")
    ap.add_argument("--password", default="", help="demo password for every account (default: random)")
    ap.add_argument("--credentials", default="", help="write the credentials here (mode 0600)")
    ap.add_argument("--undo-seconds", type=int, default=5, help="undo window restored after seeding")
    args = ap.parse_args()
    d = args.domain
    password = args.password or ("Demo-" + secrets.token_urlsafe(9))
    admin_email = f"admin@{d}"

    # ---- CLI bootstrap ------------------------------------------------------------------------
    res = run_cli(args.azmail_bin, "add-domain", "--name", d)
    if res.returncode != 0 and "exist" not in (res.stdout + res.stderr).lower():
        raise SeedError(f"azmail add-domain failed ({res.returncode}): {(res.stdout + res.stderr).strip()[-400:]}")
    res = run_cli(args.azmail_bin, "create-user", "--email", admin_email, "--name", "管理员", "--admin",
                  stdin=password + "\n")
    if res.returncode != 0:
        raise SeedError(f"azmail create-user failed ({res.returncode}): {(res.stdout + res.stderr).strip()[-400:]}"
                        "\n(already seeded? use scripts/dev.sh --reset for a fresh instance)")
    admin = login(args.api, admin_email, password)

    # ---- team ---------------------------------------------------------------------------------
    team = {"zhangsan": "张三", "lisi": "李四", "wangwu": "王五"}
    ids: dict[str, int] = {}
    for local, name in team.items():
        row = admin.call("POST", "/api/admin/users", {"email": f"{local}@{d}", "display_name": name,
                                                      "password": password}, expect=(201,))
        ids[local] = row["id"]
    admin.call("POST", "/api/admin/aliases", {
        "email": f"support@{d}", "display_name": "客服支持", "share_sent": True,
        "members": [{"user_id": ids["zhangsan"], "can_send_as": True},
                    {"user_id": ids["lisi"], "can_send_as": False}]}, expect=(201,))
    users = {local: login(args.api, f"{local}@{d}", password) for local in team}
    who = {local: addr(name, f"{local}@{d}") for local, name in team.items()}
    for client in users.values():
        client.call("PUT", "/api/settings", {"undo_send_seconds": 0})
    zs, ls, ww = users["zhangsan"], users["lisi"], users["wangwu"]
    zs.call("PUT", "/api/settings", {"signature_html": "<p>—<br>张三 · 产品部</p>", "signature_enabled": True})

    labels = {}
    for name, color in (("项目", "#1a73e8"), ("财务", "#188038"), ("待办", "#e37400")):
        labels[name] = zs.call("POST", "/api/labels", {"name": name, "color": color}, expect=(201,))["id"]

    # ---- inbound mail (through the mock, as if from the internet) -------------------------------
    now = time.time()
    inject(args.mock, **{
        "from": "李经理 <li.manager@partner.example>", "to": [f"zhangsan@{d}"],
        "subject": "合作方案 · 第三季度",
        "html": "<p>张三您好：</p><p>附件是第三季度的合作方案，请查收。下面是数据概览：</p>"
                "<p><img src=\"cid:chart-q3\" alt=\"图表\" width=\"120\"></p><p>李经理<br>合作伙伴公司</p>",
        "text": "张三您好：附件是第三季度的合作方案，请查收。",
        "attachments": [
            {"filename": "合作方案.pdf", "content_type": "application/pdf",
             "content": base64.b64encode(PDF_DEMO).decode()},
            {"filename": "chart.png", "content_type": "image/png", "content_id": "chart-q3", "inline": True,
             "content": base64.b64encode(PNG_1PX).decode()}],
        "date": int((now - 3 * 3600) * 1000)})
    inject(args.mock, **{
        "from": "王女士 <customer.wang@example.com>", "to": [f"support@{d}"],
        "subject": "订单 #20261007 无法支付", "text": "你好，我的订单一直提示支付失败，请帮忙看一下。谢谢！",
        "date": int((now - 2 * 3600) * 1000)})
    inject(args.mock, **{
        "from": "周报机器人 <weekly@reports.example>", "to": [f"zhangsan@{d}", f"lisi@{d}"],
        "subject": "周报：本周项目进度", "subject_charset": "gb18030",
        "text": "本周完成：登录页、收件箱。下周计划：搜索、标签。", "date": int((now - 5 * 3600) * 1000)})
    inject(args.mock, **{
        "from": "技术通讯 <newsletter@tech.example>", "to": [f"zhangsan@{d}"],
        "subject": "十月技术通讯：远程图片示例",
        "html": "<h2>十月技术通讯</h2><p>本期包含外部图片（默认被拦截）：</p>"
                "<p><img src=\"https://www.w3.org/Icons/w3c_home.png\" alt=\"W3C\"></p>",
        "date": int((now - 24 * 3600) * 1000)})
    inject(args.mock, **{
        "from": "账户安全中心 <security@bank-verify.example>", "to": [f"zhangsan@{d}"],
        "subject": "您的账户已被冻结，请立即验证", "text": "请点击链接验证身份……",
        "spf": "fail", "dkim": "fail", "dmarc": "fail"})

    # ---- internal conversation through the API ---------------------------------------------------
    subject = "周会纪要 · 10月8日"
    send(ls, to=[who["zhangsan"]], cc=[who["wangwu"]], subject=subject,
         html="<p>大家好，本周会议要点：</p><ol><li>搜索功能下周上线</li><li>附件预览需要再测试</li>"
              "<li>客服邮箱统一用 support@</li></ol><p>李四</p>")
    item = wait_thread(zs, subject)
    msgs = zs.call("GET", f"/api/threads/{item['id']}")["messages"]
    parent = next(m for m in msgs if m["direction"] == "in")
    reply = zs.call("POST", "/api/drafts", {"mode": "reply", "parent_message_id": parent["id"],
                                            "to": [who["lisi"]], "cc": [who["wangwu"]],
                                            "subject": "Re: " + subject,
                                            "html": "<p>收到，搜索功能我来跟进。</p>"}, expect=(201,))
    zs.call("POST", f"/api/drafts/{reply['id']}/send", {"version": reply["version"]}, expect=(202,))

    sheet = upload(ww, "预算表.csv", "text/csv", "项目,金额\n服务器,12000\n域名,300\n".encode("utf-8"))
    send(ww, to=[who["zhangsan"]], cc=[who["lisi"]], subject="预算草案（请审阅）",
         html="<p>预算草案见附件，周五前反馈。</p><p>王五</p>", attachment_ids=[sheet["id"]])
    budget = wait_thread(zs, "预算草案（请审阅）")
    zs.call("POST", "/api/threads/actions", {"thread_ids": [budget["id"]], "action": "add_label",
                                              "label_id": labels["财务"]})
    plan = wait_thread(zs, "合作方案 · 第三季度")
    zs.call("POST", "/api/threads/actions", {"thread_ids": [plan["id"]], "action": "add_label",
                                              "label_id": labels["项目"]})
    zs.call("POST", "/api/threads/actions", {"thread_ids": [plan["id"]], "action": "star"})

    send(zs, to=[addr("客户采购部", "purchase@customer.example")], subject="报价单 · AZ-2026-118",
         html="<p>您好，附上最新报价，有效期 30 天。</p>")
    send(zs, to=[addr("", "bounce@customer.example")], subject="退信示例",
         html="<p>这封邮件会被模拟服务器退回。</p>")
    zs.call("POST", "/api/drafts", {"to": [who["lisi"]], "subject": "未完成的草稿",
                                    "html": "<p>这是一封还没写完的邮件……</p>"}, expect=(201,))
    send(zs, to=[who["lisi"], who["wangwu"]], subject="下周提醒：项目评审",
         html="<p>提醒：下周三下午三点项目评审。</p>", scheduled_at=int((now + 2 * 86400) * 1000))

    for client in users.values():
        client.call("PUT", "/api/settings", {"undo_send_seconds": args.undo_seconds})

    lines = [f"AZ Mail dev accounts (password for every account: {password})",
             f"  管理员  {admin_email}  (admin)"]
    lines += [f"  {name}    {local}@{d}" for local, name in team.items()]
    lines.append(f"  alias   support@{d} → 张三 (send-as), 李四")
    print("\n".join(lines))
    if args.credentials:
        fd = os.open(args.credentials, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SeedError as exc:
        print(f"seed_dev: {exc}", file=sys.stderr)
        sys.exit(1)
