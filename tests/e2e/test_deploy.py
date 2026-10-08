#!/usr/bin/env python3
"""Checks of the shipped deployment files (deploy/, scripts/deploy_frontend.sh, docs/DEPLOY.md).

    python3 tests/e2e/test_deploy.py [-v]
    AZMAIL_BIN=backend/build/<preset>/azmail python3 tests/e2e/test_deploy.py   # + doctor checks

Python standard library only; no nginx or systemd needed. nginx's proxy_set_header inheritance is
emulated: a location inherits the server-level headers only when it defines none itself (includes
count, they are textual). The doctor checks run the real backend binary against the shipped
environment example and are skipped when no binary is found ($AZMAIL_BIN, else
backend/build/mac-debug/azmail).
"""

from __future__ import annotations

import base64
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEPLOY = REPO / "deploy"
NGINX_API = DEPLOY / "nginx-api.conf"
PROXY_SNIPPET = DEPLOY / "nginx-azmail-proxy.conf"
SNIPPET_INSTALL_PATH = "/etc/nginx/snippets/azmail-proxy.conf"
ENV_EXAMPLE = DEPLOY / "azmail.env.example"
UNIT = DEPLOY / "azmail.service"
DEPLOY_MD = REPO / "docs" / "DEPLOY.md"
DEPLOY_FRONTEND = REPO / "scripts" / "deploy_frontend.sh"

FORWARD_HEADERS = {"host", "x-real-ip", "x-forwarded-for", "x-forwarded-proto"}
# Values that never pass a client-supplied X-Forwarded-For through as the last hop.
SAFE_XFF = {"$remote_addr", "$proxy_add_x_forwarded_for"}
QUIET_LEVELS = {"crit", "alert", "emerg"}
SECRET_KEYS = ("AZMAIL_SECRET", "RESEND_API_KEY", "RESEND_WEBHOOK_SECRET", "R2_ACCOUNT_ID",
               "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")


# ---------------------------------------------------------------------------------------------
# a tiny nginx config parser: [(args, children | None)]
# ---------------------------------------------------------------------------------------------

def _tokens(text: str) -> list[str]:
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif c in "{};":
            out.append(c)
            i += 1
        elif c in "\"'":
            j, buf = i + 1, []
            while j < n and text[j] != c:
                if text[j] == "\\" and j + 1 < n:
                    j += 1
                buf.append(text[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "{};":
                j += 1
            out.append(text[i:j])
            i = j
    return out


def parse_nginx(text: str) -> list[tuple[list[str], list | None]]:
    toks = iter(_tokens(text))

    def block() -> list[tuple[list[str], list | None]]:
        items: list[tuple[list[str], list | None]] = []
        args: list[str] = []
        for tok in toks:
            if tok == ";":
                items.append((args, None))
                args = []
            elif tok == "{":
                items.append((args, block()))
                args = []
            elif tok == "}":
                return items
            else:
                args.append(tok)
        return items

    return block()


def directives(block: list, name: str) -> list[tuple[list[str], list | None]]:
    return [d for d in block if d[0] and d[0][0] == name]


def expand_includes(block: list, includes: dict[str, Path]) -> list:
    """Replaces `include <path>;` by the parsed file (``includes`` maps install paths to repo files)."""
    out = []
    for args, children in block:
        if args and args[0] == "include" and children is None:
            target = includes.get(args[1])
            if target is None:
                raise AssertionError(f"include of an unknown file: {args[1]}")
            out += expand_includes(parse_nginx(target.read_text(encoding="utf-8")), includes)
        else:
            out.append((args, children))
    return out


def effective_proxy_headers(server: list, location: list) -> dict[str, str]:
    """nginx rule: a level inherits proxy_set_header only if it defines none itself."""
    own = directives(location, "proxy_set_header")
    chosen = own if own else directives(server, "proxy_set_header")
    return {a[1].lower(): a[2] for a, _ in chosen}


def proxy_header_problems(conf_text: str, includes: dict[str, Path]) -> list[str]:
    """Every proxied location of every server must forward the client address and scheme."""
    problems = []
    for _, server in directives(parse_nginx(conf_text), "server"):
        server = expand_includes(server or [], includes)
        for args, loc in directives(server, "location"):
            loc = expand_includes(loc or [], includes)
            if not directives(loc, "proxy_pass"):
                continue
            where = " ".join(args[1:])
            hdrs = effective_proxy_headers(server, loc)
            missing = FORWARD_HEADERS - set(hdrs)
            if missing:
                problems.append(f"location {where}: no {', '.join(sorted(missing))}")
            elif hdrs["x-forwarded-for"] not in SAFE_XFF:
                problems.append(f"location {where}: X-Forwarded-For {hdrs['x-forwarded-for']}")
    return problems


def https_server(conf_text: str, includes: dict[str, Path]) -> list:
    for _, server in directives(parse_nginx(conf_text), "server"):
        if any("443" in a for a, _ in directives(server or [], "listen")):
            return expand_includes(server or [], includes)
    raise AssertionError("no listen 443 server block")


def env_values(path: Path) -> dict[str, str]:
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([A-Z][A-Z0-9_]*)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def env_defaults(path: Path) -> dict[str, str]:
    """Commented optional settings: `# KEY=default`."""
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^# ([A-Z][A-Z0-9_]*)=(\S*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


INCLUDES = {SNIPPET_INSTALL_PATH: PROXY_SNIPPET}


# ---------------------------------------------------------------------------------------------
# nginx
# ---------------------------------------------------------------------------------------------

class NginxApiTests(unittest.TestCase):
    def test_checker_detects_dropped_inheritance(self) -> None:
        # The shape of the original bug: headers at server level, one own header per location.
        broken = """
        server { listen 443 ssl;
          proxy_set_header Host $host; proxy_set_header X-Real-IP $remote_addr;
          proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
          proxy_set_header X-Forwarded-Proto $scheme;
          location /api/ { proxy_pass http://b; proxy_set_header Connection ""; }
          location /inherit/ { proxy_pass http://b; }
          location / { return 404; }
        }"""
        self.assertEqual(proxy_header_problems(broken, {}),
                         ["location /api/: no host, x-forwarded-for, x-forwarded-proto, x-real-ip"])
        passthrough = broken.replace("$proxy_add_x_forwarded_for", "$http_x_forwarded_for")
        self.assertIn("location /inherit/: X-Forwarded-For $http_x_forwarded_for",
                      proxy_header_problems(passthrough, {}))

    def test_every_proxied_location_forwards_client_address(self) -> None:
        text = NGINX_API.read_text(encoding="utf-8")
        self.assertEqual(proxy_header_problems(text, INCLUDES), [])
        server = https_server(text, INCLUDES)
        locs = {" ".join(a[1:]): expand_includes(c or [], INCLUDES) for a, c in directives(server, "location")}
        self.assertEqual(set(locs), {"= /api/ws", "= /api/webhooks/resend", "/api/files/", "/api/", "/"})
        ws = effective_proxy_headers(server, locs["= /api/ws"])
        self.assertEqual((ws["upgrade"], ws["connection"]), ("$http_upgrade", "$azmail_connection_upgrade"))
        for name in ("= /api/webhooks/resend", "/api/files/", "/api/"):
            self.assertEqual(effective_proxy_headers(server, locs[name])["connection"], "", name)

    def test_snippet_overwrites_client_forwarded_for(self) -> None:
        hdrs = {a[1].lower(): a[2] for a, _ in
                directives(parse_nginx(PROXY_SNIPPET.read_text(encoding="utf-8")), "proxy_set_header")}
        self.assertEqual(hdrs, {"host": "$host", "x-real-ip": "$remote_addr",
                                "x-forwarded-for": "$remote_addr", "x-forwarded-proto": "$scheme"})

    def test_no_server_level_proxy_headers(self) -> None:
        # They would be dead code (every location defines its own) and invite the original bug.
        server = https_server(NGINX_API.read_text(encoding="utf-8"), INCLUDES)
        self.assertEqual(directives(server, "proxy_set_header"), [])

    def test_signed_url_location_does_not_log_errors(self) -> None:
        server = https_server(NGINX_API.read_text(encoding="utf-8"), INCLUDES)
        access = directives(server, "access_log")
        self.assertTrue(access and access[0][0][2] == "azmail_noquery", access)
        files = next(c for a, c in directives(server, "location") if a[1:] == ["/api/files/"])
        levels = [a[2] for a, _ in directives(files, "error_log") if len(a) >= 3]
        self.assertTrue(levels and all(lv in QUIET_LEVELS for lv in levels), levels)

    def test_snippet_install_is_documented(self) -> None:
        doc = DEPLOY_MD.read_text(encoding="utf-8")
        self.assertIn(f"deploy/nginx-azmail-proxy.conf {SNIPPET_INSTALL_PATH}", doc)
        self.assertIn(SNIPPET_INSTALL_PATH, NGINX_API.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------------------------
# environment example + systemd unit
# ---------------------------------------------------------------------------------------------

class EnvExampleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.values = env_values(ENV_EXAMPLE)

    def test_secrets_are_obvious_placeholders(self) -> None:
        for key in SECRET_KEYS:
            with self.subTest(key=key):
                self.assertIn(key, self.values)
                self.assertIn("CHANGE_ME", self.values[key])
        # Rejected by the backend's validation (AZMAIL_SECRET: placeholder marker CHANGE_ME;
        # RESEND_WEBHOOK_SECRET: no whsec_ prefix), so an unedited copy can never start a service
        # with a publicly known key. DoctorTests checks this against the real binary.
        self.assertTrue(self.values["AZMAIL_SECRET"].startswith("CHANGE_ME_"))
        self.assertFalse(self.values["RESEND_WEBHOOK_SECRET"].startswith("whsec_"))
        self.assertIn("openssl rand -hex 32", ENV_EXAMPLE.read_text(encoding="utf-8"))

    def test_data_paths_are_absolute(self) -> None:
        for key in ("AZMAIL_DATA_DIR", "AZMAIL_DB_PATH"):
            with self.subTest(key=key):
                self.assertTrue(self.values.get(key, "").startswith("/"), self.values.get(key))
        self.assertTrue(self.values["AZMAIL_DB_PATH"].startswith(self.values["AZMAIL_DATA_DIR"] + "/"))
        unit = UNIT.read_text(encoding="utf-8")
        self.assertIn(f"WorkingDirectory={self.values['AZMAIL_DATA_DIR']}\n", unit)

    def test_stop_timeout_covers_graceful_shutdown(self) -> None:
        grace = int(env_defaults(ENV_EXAMPLE)["AZMAIL_SHUTDOWN_GRACE_SEC"])
        hpp = (REPO / "backend" / "src" / "config.hpp").read_text(encoding="utf-8")
        m = re.search(r"int shutdown_grace_sec = (\d+);", hpp)
        self.assertTrue(m, "shutdown_grace_sec default not found in backend/src/config.hpp")
        self.assertEqual(grace, int(m.group(1)), "env example and config.hpp disagree")
        m = re.search(r"^TimeoutStopSec=(\d+)$", UNIT.read_text(encoding="utf-8"), re.M)
        self.assertTrue(m)
        stop = int(m.group(1))
        self.assertGreaterEqual(stop, grace + 10, "TimeoutStopSec must exceed the grace by >= 10 s")
        self.assertIn(f"TimeoutStopSec={stop}", DEPLOY_MD.read_text(encoding="utf-8"))
        self.assertIn(f"TimeoutStopSec={stop}", ENV_EXAMPLE.read_text(encoding="utf-8"))


def _azmail_bin() -> Path | None:
    p = Path(os.environ.get("AZMAIL_BIN") or REPO / "backend" / "build" / "mac-debug" / "azmail")
    if not p.is_absolute():
        p = REPO / p
    return p if p.is_file() and os.access(p, os.X_OK) else None


@unittest.skipIf(_azmail_bin() is None, "no azmail binary ($AZMAIL_BIN / backend/build/mac-debug)")
class DoctorTests(unittest.TestCase):
    """`azmail doctor --offline` with the shipped example: placeholders must fail validation."""

    def doctor(self, env_file: Path) -> subprocess.CompletedProcess:
        tmp = Path(tempfile.mkdtemp(prefix="azmail-deploy-test-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("AZMAIL_", "RESEND_", "R2_"))}
        # Only the paths are redirected (they point at /var/lib/azmail); everything else verbatim.
        return subprocess.run([str(_azmail_bin()), "--env-file", str(env_file),
                               "--data-dir", str(tmp / "data"), "--db-path", str(tmp / "data" / "azmail.db"),
                               "doctor", "--offline"], capture_output=True, text=True, timeout=60,
                              env=env, cwd=tmp)

    def test_unedited_example_is_rejected(self) -> None:
        res = self.doctor(ENV_EXAMPLE)
        out = res.stdout + res.stderr
        self.assertNotEqual(res.returncode, 0, out)
        config_fails = [l for l in out.splitlines() if l.startswith("[FAIL] configuration")]
        self.assertTrue(any("AZMAIL_SECRET" in l for l in config_fails), out)
        self.assertTrue(any("RESEND_WEBHOOK_SECRET" in l for l in config_fails), out)
        self.assertNotIn("[ OK ] configuration", out)

    @staticmethod
    def filled_values() -> dict[str, str]:
        return {
            "AZMAIL_SECRET": secrets.token_hex(32),
            "RESEND_API_KEY": "re_" + secrets.token_urlsafe(24),
            "RESEND_WEBHOOK_SECRET": "whsec_" + base64.b64encode(secrets.token_bytes(24)).decode(),
            "R2_ACCOUNT_ID": secrets.token_hex(16),
            "R2_ACCESS_KEY_ID": secrets.token_hex(16),
            "R2_SECRET_ACCESS_KEY": secrets.token_hex(32),
        }

    def example_with(self, filled: dict[str, str]) -> Path:
        lines = []
        for line in ENV_EXAMPLE.read_text(encoding="utf-8").splitlines():
            key = line.split("=", 1)[0]
            lines.append(f"{key}={filled[key]}" if key in filled else line)
        tmp = Path(tempfile.mkdtemp(prefix="azmail-deploy-env-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        env_file = tmp / "azmail.env"
        env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return env_file

    def test_secret_placeholder_alone_is_rejected(self) -> None:
        # Everything filled in except AZMAIL_SECRET: the placeholder is >= 32 bytes, so only the
        # backend's placeholder check (F2) keeps a publicly known signing key out of production.
        filled = self.filled_values()
        del filled["AZMAIL_SECRET"]
        res = self.doctor(self.example_with(filled))
        out = res.stdout + res.stderr
        self.assertNotEqual(res.returncode, 0, out)
        config_fails = [l for l in out.splitlines() if l.startswith("[FAIL] configuration")]
        self.assertTrue(any("AZMAIL_SECRET" in l for l in config_fails), out)

    def test_every_openssl_rand_hex_32_value_is_accepted(self) -> None:
        # `openssl rand -hex 32` (the documented command) must always pass, including the ~1 in 16
        # outputs whose last hex digit equals the first (a repetition-based entropy estimate would
        # see a 63-character period and reject them).
        for secret in ("a" + secrets.token_hex(31) + "a",      # 1 + 62 + 1 hex digits
                       "0f" + secrets.token_hex(30) + "0f"):   # 2 + 60 + 2
            self.assertEqual(len(secret), 64)
            filled = self.filled_values()
            filled["AZMAIL_SECRET"] = secret
            with self.subTest(head=secret[:2], tail=secret[-2:]):
                out = self.doctor(self.example_with(filled))
                self.assertIn("[ OK ] configuration", out.stdout + out.stderr)

    def test_filled_in_example_passes_configuration(self) -> None:
        # Control: the placeholders are the only configuration problems of the example.
        out = self.doctor(self.example_with(self.filled_values()))
        self.assertIn("[ OK ] configuration", out.stdout + out.stderr)


# ---------------------------------------------------------------------------------------------
# frontend deployment (config.js survives upgrades)
# ---------------------------------------------------------------------------------------------

@unittest.skipIf(shutil.which("rsync") is None or shutil.which("bash") is None, "rsync/bash missing")
class DeployFrontendTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="azmail-deploy-fe-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.dist = self.tmp / "dist"
        (self.dist / "assets").mkdir(parents=True)
        (self.dist / "index.html").write_text("<!doctype html>v1", encoding="utf-8")
        (self.dist / "assets" / "app-v1.js").write_text("v1", encoding="utf-8")
        (self.dist / "config.js").write_text('window.__AZMAIL_CONFIG__ = { apiBase: "" };\n', encoding="utf-8")
        self.root = self.tmp / "www"

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(DEPLOY_FRONTEND), "--dist", str(self.dist), "--root", str(self.root),
                               *args], capture_output=True, text=True, timeout=60)

    def config(self) -> str:
        return (self.root / "config.js").read_text(encoding="utf-8")

    def test_first_install_needs_api_base(self) -> None:
        res = self.run_script()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("--api-base", res.stderr)
        self.assertFalse((self.root / "index.html").exists())

    def test_install_then_upgrade_keeps_config(self) -> None:
        res = self.run_script("--api-base", "https://mail-api.example.com/")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn('apiBase: "https://mail-api.example.com" }', self.config())
        self.assertEqual((self.root / "index.html").read_text(encoding="utf-8"), "<!doctype html>v1")
        self.assertEqual(oct((self.root / "config.js").stat().st_mode & 0o777), "0o644")
        # upgrade: new build (with its dev config.js), a stale asset in the web root
        (self.dist / "assets" / "app-v1.js").unlink()
        (self.dist / "assets" / "app-v2.js").write_text("v2", encoding="utf-8")
        (self.dist / "index.html").write_text("<!doctype html>v2", encoding="utf-8")
        later = time.time() + 60  # a later build (rsync's quick check compares size + mtime)
        for f in self.dist.rglob("*"):
            os.utime(f, (later, later))
        res = self.run_script("--dry-run")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertNotIn("config.js", res.stdout)
        self.assertTrue((self.root / "assets" / "app-v1.js").exists(), "dry run must not change anything")
        res = self.run_script()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("https://mail-api.example.com", self.config())
        self.assertEqual((self.root / "index.html").read_text(encoding="utf-8"), "<!doctype html>v2")
        self.assertFalse((self.root / "assets" / "app-v1.js").exists(), "--delete removes stale files")
        self.assertTrue((self.root / "assets" / "app-v2.js").exists())
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["assets", "config.js", "index.html"])
        # an explicit --api-base on an upgrade replaces the environment's value
        res = self.run_script("--api-base", "https://api2.example.com:8443")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn('"https://api2.example.com:8443"', self.config())

    def test_rejects_non_origin_api_base(self) -> None:
        for bad in ("mail-api.example.com", "https://mail-api.example.com/api", 'https://x.example.com";alert(1)//',
                    "javascript:alert(1)", "https://"):
            with self.subTest(bad=bad):
                res = self.run_script("--api-base", bad)
                self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                self.assertFalse((self.root / "config.js").exists())

    def test_warns_about_same_origin_config(self) -> None:
        self.root.mkdir()
        (self.root / "config.js").write_text('window.__AZMAIL_CONFIG__ = { apiBase: "" };\n', encoding="utf-8")
        res = self.run_script()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("warning", res.stderr)

    def test_docs_never_rsync_over_config(self) -> None:
        doc = DEPLOY_MD.read_text(encoding="utf-8")
        cmds = [l for l in doc.splitlines() if re.search(r"rsync\b.*\bdist/", l)]
        self.assertTrue(cmds, "DEPLOY.md shows the rsync command")
        for line in cmds:
            self.assertIn("--exclude=/config.js", line, line)
        self.assertIn("scripts/deploy_frontend.sh --api-base", doc)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
