"""E2E environment: temp data dir, mock Resend/R2 and the azmail backend on random ports.

Backend configuration comes from the environment variable names documented in
backend/src/config.hpp (AZMAIL_* / RESEND_* / R2_*); CLI subcommands from
backend/src/app/cli.hpp (migrate, add-domain, create-user, doctor, …).
"""

from __future__ import annotations

import base64
import json
import os
import secrets
import sqlite3
import sys
import time
from pathlib import Path
from typing import Any, Callable

from .api import Api, http_request
from .mock import MockClient
from .proc import Proc, free_port, run_cli, tail_file
from .signing import SignedUrls, detect
from .ws import WsClient

REPO = Path(__file__).resolve().parents[3]

DOMAIN = "corp.test"            # team domain created by the CLI bootstrap (E2E 01)
EXTRA_DOMAIN = "branch.corp.test"  # created through the admin API (E2E 02)
EXTERNAL = "ext.test"           # "the internet"
ALLOWED_ORIGIN = "http://localhost:5173"
SECOND_ORIGIN = "https://mail.corp.test"
ADMIN_EMAIL = f"admin@{DOMAIN}"
R2_BUCKET = "azmail-e2e"


class SetupError(RuntimeError):
    """The environment could not be brought up (reported with log tails)."""


class Skip(Exception):
    """Raised by a scenario that does not apply to this run (e.g. R2-only on local)."""


class Env:
    def __init__(self, *, azmail_bin: Path, blob_backend: str, root: Path, verbose: bool = False) -> None:
        self.bin = azmail_bin
        self.blob_backend = blob_backend
        self.root = root
        self.verbose = verbose
        self.data_dir = root / "data"
        self.db_path = self.data_dir / "azmail.db"
        self.logs = root / "logs"
        self.logs.mkdir(parents=True, exist_ok=True)
        self.api_port = free_port()
        self.mock_port = 0
        self.s3_port = 0
        self.server_secret = "e2e." + secrets.token_hex(24) + ".secret"
        self.webhook_secret = "whsec_" + base64.b64encode(secrets.token_bytes(24)).decode()
        self.resend_key = "re_e2e_" + secrets.token_hex(12)
        self.s3_access_key = "e2e-access-" + secrets.token_hex(4)
        self.s3_secret_key = "e2e-secret-" + secrets.token_hex(16)
        self.admin_password = "Adm1n-" + secrets.token_urlsafe(12)
        self.mock_proc: Proc | None = None
        self.backend: Proc | None = None
        self.overrides: dict[str, str] = {}
        self.cli_log = self.logs / "cli.log"

    # -- URLs -----------------------------------------------------------------------------------
    @property
    def api_base(self) -> str:
        return f"http://127.0.0.1:{self.api_port}"

    @property
    def ws_url(self) -> str:
        return f"ws://127.0.0.1:{self.api_port}/api/ws"

    @property
    def mock_base(self) -> str:
        return f"http://127.0.0.1:{self.mock_port}"

    @property
    def s3_base(self) -> str:
        return f"http://127.0.0.1:{self.s3_port}"

    # -- mock -----------------------------------------------------------------------------------
    def start_mock(self, timeout: float = 15.0) -> None:
        port_file = self.root / "mock-ports.json"
        argv = [sys.executable, "-m", "tools.mock_resend.server", "--port", "0", "--s3-port", "0",
                "--port-file", str(port_file), "--api-key", self.resend_key,
                "--webhook-url", f"{self.api_base}/api/webhooks/resend",
                "--webhook-secret", self.webhook_secret,
                "--local-domains", f"{DOMAIN},{EXTRA_DOMAIN}",
                "--s3-access-key", self.s3_access_key, "--s3-secret-key", self.s3_secret_key,
                "--s3-bucket", R2_BUCKET]
        env = dict(os.environ)
        env["PYTHONPATH"] = str(REPO) + os.pathsep + env.get("PYTHONPATH", "")
        self.mock_proc = Proc("mock_resend", argv, env, self.logs / "mock.log", cwd=str(REPO),
                              redact=[self.resend_key, self.webhook_secret, self.s3_secret_key]).start()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if port_file.exists():
                ports = json.loads(port_file.read_text())
                self.mock_port, self.s3_port = int(ports["port"]), int(ports["s3_port"])
                if http_request("GET", f"{self.mock_base}/_mock/health").status == 200:
                    return
            if not self.mock_proc.alive():
                break
            time.sleep(0.1)
        raise SetupError("mock_resend did not start:\n" + self.mock_proc.log_tail(40))

    def mock_client(self) -> MockClient:
        return MockClient(self.mock_base, self.resend_key, self.s3_base)

    # -- backend environment -------------------------------------------------------------------------
    def backend_env(self, overrides: dict[str, str] | None = None) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("AZMAIL_", "RESEND_", "R2_"))}
        env.update({
            "AZMAIL_LISTEN_ADDRESS": "127.0.0.1",
            "AZMAIL_PORT": str(self.api_port),
            "AZMAIL_PUBLIC_API_URL": self.api_base,
            "AZMAIL_CORS_ORIGINS": f"{ALLOWED_ORIGIN},{SECOND_ORIGIN}",
            "AZMAIL_DATA_DIR": str(self.data_dir),
            "AZMAIL_DB_PATH": str(self.db_path),
            "AZMAIL_SECRET": self.server_secret,
            "AZMAIL_LOCAL_DOMAINS": f"{DOMAIN},{EXTRA_DOMAIN}",
            "AZMAIL_LOG_LEVEL": "debug",
            "AZMAIL_UNDO_SEND_SECONDS": "0",       # scenarios that test undo set 5 s explicitly
            "AZMAIL_SCHEDULE_MIN_LEAD_SEC": "5",   # local scheduling (E2E 17) without a 60 s wait
            "AZMAIL_POLL_INTERVAL_SEC": "3600",    # only /api/admin/sync polls (E2E 22)
            "AZMAIL_RECONCILE_INTERVAL_SEC": "15",
            "AZMAIL_SHUTDOWN_GRACE_SEC": "5",
            "AZMAIL_ALLOW_INSECURE_HTTP": "1",     # http:// mock endpoints
            "RESEND_API_KEY": self.resend_key,
            "RESEND_API_BASE": self.mock_base,
            "RESEND_WEBHOOK_SECRET": self.webhook_secret,
            "RESEND_TIMEOUT_SEC": "4",             # E2E 20 "one timeout" fault holds 8 s
            "RESEND_RATE_RPS": "8",
            "AZMAIL_BLOB_BACKEND": self.blob_backend,
            "AZMAIL_FILES_DELIVERY": "proxy",
        })
        if self.blob_backend == "r2":
            env.update({
                "R2_ACCOUNT_ID": "e2e0000000000000000000000000000",
                "R2_ACCESS_KEY_ID": self.s3_access_key,
                "R2_SECRET_ACCESS_KEY": self.s3_secret_key,
                "R2_BUCKET": R2_BUCKET,
                "R2_ENDPOINT": self.s3_base,
                "R2_PREFIX": "azmail/",
                "R2_PRESIGN_TTL_SEC": "300",
            })
        env.update(self.overrides)
        env.update(overrides or {})
        return env

    # -- CLI --------------------------------------------------------------------------------------
    def cli(self, *args: str, stdin: str | None = None, timeout: float = 120.0,
            overrides: dict[str, str] | None = None) -> Any:
        res = run_cli([str(self.bin), *args], self.backend_env(overrides), stdin=stdin, timeout=timeout)
        with open(self.cli_log, "a", encoding="utf-8") as fh:
            fh.write(f"\n$ azmail {' '.join(args)}  → exit {res.returncode}\n{res.stdout}{res.stderr}")
        return res

    def bootstrap(self) -> None:
        """E2E 01 setup: migrate, add-domain, create-user --admin (password on stdin)."""
        steps = [
            (["migrate"], None),
            (["add-domain", "--name", DOMAIN], None),
            (["create-user", "--email", ADMIN_EMAIL, "--name", "管理员", "--admin"],
             self.admin_password + "\n"),
        ]
        for args, stdin in steps:
            res = self.cli(*args, stdin=stdin)
            if res.returncode != 0:
                raise SetupError(f"`azmail {' '.join(args)}` failed (exit {res.returncode}):\n"
                                 f"{(res.stdout + res.stderr).strip()[-2000:]}")
        (self.root / "credentials.txt").write_text(
            f"admin: {ADMIN_EMAIL} / {self.admin_password}\napi: {self.api_base}\n"
            f"mock: {self.mock_base}  s3: {self.s3_base}\n", encoding="utf-8")

    # -- backend process ------------------------------------------------------------------------------
    def start_backend(self, overrides: dict[str, str] | None = None, timeout: float = 30.0) -> None:
        if overrides is not None:
            self.overrides = dict(overrides)
        self.backend = Proc("azmail", [str(self.bin), "serve"], self.backend_env(),
                            self.logs / "backend.log").start()
        self.wait_healthy(timeout)

    def wait_healthy(self, timeout: float = 30.0) -> None:
        deadline = time.monotonic() + timeout
        last = "no response"
        while time.monotonic() < deadline:
            assert self.backend is not None
            if not self.backend.alive():
                raise SetupError(f"azmail serve exited with code {self.backend.returncode} before "
                                 "becoming healthy (see the backend log below)")
            try:
                resp = http_request("GET", f"{self.api_base}/api/health", timeout=2)
                if resp.status == 200:
                    return
                last = f"HTTP {resp.status}: {resp.short(200)}"
            except OSError as exc:
                last = f"{type(exc).__name__}: {exc}"
            time.sleep(0.2)
        raise SetupError(f"azmail serve not healthy after {timeout:.0f}s ({last}); "
                         "see the backend log below")

    def stop_backend(self) -> int | None:
        return self.backend.stop(timeout=20) if self.backend else None

    def kill_backend(self) -> None:
        if self.backend:
            self.backend.kill()

    def restart_backend(self, overrides: dict[str, str] | None = None) -> None:
        """Graceful stop, then start with ``overrides`` (None keeps the current ones)."""
        self.stop_backend()
        self.start_backend(overrides if overrides is not None else self.overrides)

    def backend_alive(self) -> bool:
        return self.backend is not None and self.backend.alive()

    def stop_all(self) -> None:
        for proc in (self.backend, self.mock_proc):
            if proc is not None:
                try:
                    proc.stop(timeout=10)
                except Exception:  # noqa: BLE001 — best-effort cleanup
                    proc.kill()

    def diagnostics(self, lines: int = 40) -> str:
        out = [f"--- backend log (last {lines} lines: {self.logs / 'backend.log'}) ---",
               tail_file(self.logs / "backend.log", lines).rstrip()]
        mock_dead = self.mock_proc is not None and not self.mock_proc.alive()
        if mock_dead or self.verbose:
            state = f"exited {self.mock_proc.returncode}" if mock_dead and self.mock_proc else "running"
            out += [f"--- mock log ({state}; last 15 lines: {self.logs / 'mock.log'}) ---",
                    tail_file(self.logs / "mock.log", 15).rstrip()]
        return "\n".join(out)

    def db(self) -> sqlite3.Connection:
        """Direct SQLite access (test-only manipulation, e.g. backdating for purge/GC in E2E 33)."""
        conn = sqlite3.connect(str(self.db_path), timeout=15)
        conn.execute("PRAGMA busy_timeout=15000")
        return conn


class Ctx:
    """Passed to every scenario: env, API clients, mock client, shared state, helpers."""

    def __init__(self, env: Env) -> None:
        self.env = env
        self.api = Api(env.api_base)
        self.mock = env.mock_client()
        self.state: dict[str, Any] = {}
        self._admin: Api | None = None
        self._deferred: list[Callable[[], None]] = []
        self._signer: SignedUrls | None = None
        self.scenario = ""

    # -- clients --------------------------------------------------------------------------------
    @property
    def admin(self) -> Api:
        if self._admin is not None and self._admin.request("GET", "/api/auth/me").status == 200:
            return self._admin
        self._admin, _ = self.api.login(ADMIN_EMAIL, self.env.admin_password)
        return self._admin

    def team(self) -> Any:
        from .flows import ensure_team  # local import: flows depends on env

        return ensure_team(self)

    def ws(self, api: Api | None = None, origin: str | None = ALLOWED_ORIGIN) -> WsClient:
        client = WsClient(self.env.ws_url, origin=origin).connect()
        self.defer(client.close)
        if api is not None:
            assert api.token, "ws(): api has no token"
            client.auth(api.token)
        return client

    # -- helpers --------------------------------------------------------------------------------
    @staticmethod
    def uniq(prefix: str = "T") -> str:
        return f"{prefix}-{secrets.token_hex(3)}"

    def log(self, msg: str) -> None:
        if self.env.verbose:
            print(f"      · {msg}", flush=True)

    def defer(self, fn: Callable[[], None]) -> None:
        """Runs ``fn`` after the scenario (LIFO), even when it failed."""
        self._deferred.append(fn)

    def run_deferred(self) -> list[str]:
        errors = []
        while self._deferred:
            fn = self._deferred.pop()
            try:
                fn()
            except Exception as exc:  # noqa: BLE001 — cleanup must not mask the result
                errors.append(f"{type(exc).__name__}: {exc}")
        return errors

    def mark(self) -> int:
        """Mock sequence cursor: pass to mock.sent/posts/emails(since=…) to see only newer items."""
        return self.mock.seq()

    def signer(self, observed_url: str) -> SignedUrls | None:
        if self._signer is None:
            self._signer = detect(self.env.server_secret, self.env.api_base, observed_url)
        return self._signer

    def restart_backend(self, **overrides: str) -> None:
        self.env.restart_backend(dict(overrides))
