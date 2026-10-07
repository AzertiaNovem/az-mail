"""Child-process helpers: start/stop/kill with log files, CLI runs, free ports."""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from collections import deque
from pathlib import Path
from typing import Mapping, Sequence


def free_port() -> int:
    """An ephemeral TCP port that is free right now on 127.0.0.1."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def tail_file(path: Path, lines: int = 60) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return "".join(deque(fh, maxlen=lines))
    except FileNotFoundError:
        return "(no log file)\n"


class Proc:
    """A long-running child process whose stdout+stderr go to ``log_path`` (appended)."""

    def __init__(self, name: str, argv: Sequence[str], env: Mapping[str, str], log_path: Path,
                 cwd: str | None = None, redact: Sequence[str] = ()) -> None:
        self.name = name
        self.argv = list(argv)
        self.redact = [r for r in redact if r]
        self.env = dict(env)
        self.log_path = log_path
        self.cwd = cwd
        self.popen: subprocess.Popen[bytes] | None = None
        self._log_fh = None

    def start(self) -> "Proc":
        if self.alive():
            raise RuntimeError(f"{self.name} already running")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_fh = open(self.log_path, "ab", buffering=0)
        shown = " ".join("***" if a in self.redact else a for a in self.argv)
        self._log_fh.write(f"\n===== start {self.name} {time.strftime('%H:%M:%S')}: {shown}\n".encode())
        # Own session: Ctrl-C in the harness terminal does not hit children; we stop them.
        self.popen = subprocess.Popen(self.argv, env=self.env, cwd=self.cwd, stdin=subprocess.DEVNULL,
                                      stdout=self._log_fh, stderr=subprocess.STDOUT,
                                      start_new_session=True)
        return self

    @property
    def pid(self) -> int | None:
        return self.popen.pid if self.popen else None

    @property
    def returncode(self) -> int | None:
        return self.popen.poll() if self.popen else None

    def alive(self) -> bool:
        return self.popen is not None and self.popen.poll() is None

    def stop(self, timeout: float = 15.0) -> int | None:
        """SIGTERM, then SIGKILL after ``timeout`` seconds. Returns the exit code."""
        if self.popen is None:
            return None
        if self.popen.poll() is None:
            try:
                self.popen.send_signal(signal.SIGTERM)
                self.popen.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.kill()
            except ProcessLookupError:
                pass
        self._close_log()
        return self.popen.returncode

    def kill(self) -> None:
        """SIGKILL (crash simulation) and reap."""
        if self.popen is not None and self.popen.poll() is None:
            try:
                os.killpg(self.popen.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                self.popen.kill()
            self.popen.wait(timeout=10)
        self._close_log()

    def _close_log(self) -> None:
        if self._log_fh is not None:
            try:
                self._log_fh.write(f"===== {self.name} exited ({self.returncode})\n".encode())
                self._log_fh.close()
            except OSError:
                pass
            self._log_fh = None

    def log_tail(self, lines: int = 60) -> str:
        return tail_file(self.log_path, lines)


def run_cli(argv: Sequence[str], env: Mapping[str, str], stdin: str | None = None,
            timeout: float = 120.0, cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    """Runs a short-lived command, capturing output (never raises on a non-zero exit)."""
    try:
        return subprocess.run(list(argv), env=dict(env), input=stdin, capture_output=True,
                              text=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired as exc:
        return subprocess.CompletedProcess(list(argv), returncode=-9,
                                           stdout=(exc.stdout or b"").decode("utf-8", "replace")
                                           if isinstance(exc.stdout, bytes) else (exc.stdout or ""),
                                           stderr=f"timed out after {timeout}s")
    except OSError as exc:
        return subprocess.CompletedProcess(list(argv), returncode=-1, stdout="",
                                           stderr=f"cannot execute: {exc}")
