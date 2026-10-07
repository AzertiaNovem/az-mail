#!/usr/bin/env python3
"""AZ Mail end-to-end test runner (Python standard library only).

Starts the mock Resend/R2 server and the real ``azmail`` binary on random ports with a temp data
directory, bootstraps it through the CLI (migrate, add-domain, create-user --admin) and runs the
scenarios in tests/e2e/scenarios (DESIGN §6 table 01–34 + Addendum A scenario 35) in order,
sharing one environment. The whole suite runs with AZMAIL_BLOB_BACKEND=r2 against the mock S3
endpoint; afterwards a short smoke subset runs on a fresh environment with the local backend.

Examples:
  python3 tests/e2e/run.py                      # everything (r2) + local smoke
  python3 tests/e2e/run.py -k s03 -k undo       # scenarios whose name contains s03 or undo
  python3 tests/e2e/run.py --blob-backend local # whole suite on the local blob store
  python3 tests/e2e/run.py --list
  python3 tests/e2e/run.py --keep -v            # keep temp dir + logs, verbose steps

Exit status: 0 = all selected scenarios passed (skips allowed), 1 = failures, 2 = setup/usage error.
"""

from __future__ import annotations

import argparse
import importlib
import os
import shutil
import signal
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(REPO))

from lib.env import Ctx, Env, SetupError, Skip  # noqa: E402

DEFAULT_BIN = REPO / "backend" / "build" / "mac-debug" / "azmail"


class ScenarioTimeout(Exception):
    pass


@dataclass
class Scenario:
    id: str
    name: str
    title: str
    module: ModuleType
    smoke: bool
    timeout: int


@dataclass
class Result:
    scenario: Scenario
    status: str  # PASS | FAIL | ERROR | SKIP
    seconds: float
    detail: str = ""


def discover() -> list[Scenario]:
    out = []
    for path in sorted((HERE / "scenarios").glob("s[0-9][0-9]_*.py")):
        mod = importlib.import_module(f"scenarios.{path.stem}")
        title = getattr(mod, "TITLE", None) or (mod.__doc__ or path.stem).strip().splitlines()[0]
        out.append(Scenario(id=path.stem[:3], name=path.stem, title=title, module=mod,
                            smoke=bool(getattr(mod, "SMOKE", False)),
                            timeout=int(getattr(mod, "TIMEOUT", 120))))
    return out


def select(scenarios: list[Scenario], patterns: list[str]) -> list[Scenario]:
    pats = [p.strip().lower() for raw in patterns for p in raw.split(",") if p.strip()]
    if not pats:
        return scenarios
    return [s for s in scenarios if any(p in s.name.lower() or p in s.title.lower() for p in pats)]


def _on_alarm(signum, frame):  # noqa: ANN001
    raise ScenarioTimeout("scenario timed out")


def _format_failure(exc: BaseException) -> str:
    frames = traceback.extract_tb(exc.__traceback__)
    ours = [f for f in frames if "/tests/e2e/" in f.filename.replace(os.sep, "/")
            and not f.filename.endswith("run.py")] or frames
    lines = traceback.format_list(ours[-6:])
    msg = f"{type(exc).__name__}: {exc}"
    return "".join(lines).rstrip() + "\n" + msg


def run_suite(args: argparse.Namespace, scenarios: list[Scenario], backend: str, label: str) -> list[Result]:
    root = Path(tempfile.mkdtemp(prefix=f"azmail-e2e-{backend}-"))
    env = Env(azmail_bin=args.azmail_bin, blob_backend=backend, root=root, verbose=args.verbose)
    results: list[Result] = []
    print(f"\n== {label}: {len(scenarios)} scenario(s), blob backend {backend}, temp {root}", flush=True)
    try:
        try:
            t0 = time.monotonic()
            env.start_mock()
            print(f"   mock_resend  API {env.mock_base}  R2 {env.s3_base}", flush=True)
            env.bootstrap()
            print("   bootstrap    azmail migrate / add-domain / create-user --admin: ok", flush=True)
            env.start_backend()
            print(f"   azmail serve {env.api_base}  (ready in {time.monotonic() - t0:.1f}s)", flush=True)
        except SetupError as exc:
            print(f"\n!! SETUP FAILED: {exc}\n{env.diagnostics(30)}", flush=True)
            return [Result(s, "ERROR", 0.0, "environment setup failed") for s in scenarios]
        ctx = Ctx(env)
        for scen in scenarios:
            results.append(run_one(args, env, ctx, scen))
            if args.fail_fast and results[-1].status in ("FAIL", "ERROR"):
                for rest in scenarios[len(results):]:
                    results.append(Result(rest, "SKIP", 0.0, "not run (--fail-fast)"))
                break
    finally:
        env.stop_all()
        if args.keep:
            print(f"   kept {root} (logs in {env.logs})", flush=True)
        else:
            shutil.rmtree(root, ignore_errors=True)
    return results


def run_one(args: argparse.Namespace, env: Env, ctx: Ctx, scen: Scenario) -> Result:
    if not env.backend_alive():
        try:
            env.start_backend({})
        except SetupError as exc:
            res = Result(scen, "ERROR", 0.0, f"backend not running and restart failed: {exc}")
            _print_result(res)
            return res
    try:
        ctx.mock.soft_reset()
    except Exception as exc:  # noqa: BLE001
        res = Result(scen, "ERROR", 0.0, f"mock reset failed: {exc}")
        _print_result(res)
        return res
    ctx.scenario = scen.id
    print(f"[{scen.id}] {scen.title} ...", flush=True)
    t0 = time.monotonic()
    timeout = int(scen.timeout * args.timeout_scale)
    signal.signal(signal.SIGALRM, _on_alarm)
    signal.alarm(max(1, timeout))
    status, detail = "PASS", ""
    try:
        scen.module.run(ctx)
    except Skip as exc:
        status, detail = "SKIP", str(exc)
    except ScenarioTimeout as exc:
        status, detail = "FAIL", f"timed out after {timeout}s\n" + _format_failure(exc)
    except AssertionError as exc:
        status, detail = "FAIL", _format_failure(exc)
    except Exception as exc:  # noqa: BLE001 — harness errors are reported, not fatal
        status, detail = "ERROR", _format_failure(exc)
    finally:
        signal.alarm(0)
    cleanup_errors = ctx.run_deferred()
    if env.overrides:  # the scenario restarted the backend with overrides: back to defaults
        try:
            env.restart_backend({})
        except SetupError as exc:
            cleanup_errors.append(f"restart with default config failed: {exc}")
    if cleanup_errors and status == "PASS":
        detail = "cleanup: " + "; ".join(cleanup_errors)
    if status in ("FAIL", "ERROR"):
        detail += "\n" + env.diagnostics(args.log_lines)
    res = Result(scen, status, time.monotonic() - t0, detail)
    _print_result(res)
    return res


def _print_result(res: Result) -> None:
    mark = {"PASS": "PASS", "FAIL": "FAIL", "ERROR": "ERROR", "SKIP": "SKIP"}[res.status]
    print(f"[{res.scenario.id}] {mark} ({res.seconds:.1f}s)", flush=True)
    if res.detail and res.status != "PASS":
        for line in res.detail.rstrip().splitlines():
            print("      " + line)
    elif res.detail:
        print("      " + res.detail)


def summarize(label: str, results: list[Result]) -> bool:
    if results and all(r.detail == "environment setup failed" for r in results):
        print(f"== {label}: environment setup failed — {len(results)} scenario(s) not run")
        return False
    counts = {k: sum(1 for r in results if r.status == k) for k in ("PASS", "FAIL", "ERROR", "SKIP")}
    print(f"== {label}: {counts['PASS']} passed, {counts['FAIL']} failed, {counts['ERROR']} errors, "
          f"{counts['SKIP']} skipped")
    bad = [r for r in results if r.status in ("FAIL", "ERROR")]
    for r in bad:
        print(f"   {r.status:5} {r.scenario.name}")
    return not bad


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", action="append", default=[], metavar="PATTERN",
                    help="run scenarios whose file name/title contains PATTERN (repeatable, comma list)")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    ap.add_argument("--azmail-bin", type=Path,
                    default=Path(os.environ.get("AZMAIL_BIN") or DEFAULT_BIN),
                    help=f"backend binary (default $AZMAIL_BIN or {DEFAULT_BIN.relative_to(REPO)})")
    ap.add_argument("--blob-backend", choices=("r2", "local"), default="r2")
    ap.add_argument("--no-local-smoke", action="store_true",
                    help="skip the short local-blob-store smoke run after an r2 run")
    ap.add_argument("--keep", action="store_true", help="keep the temp dir (data, logs, credentials)")
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--timeout-scale", type=float, default=1.0, help="multiply scenario timeouts")
    ap.add_argument("--log-lines", type=int, default=40, help="backend log lines shown per failure")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    scenarios = discover()
    selected = select(scenarios, args.k)
    if args.list:
        for s in selected:
            print(f"{s.name:32} {'[smoke] ' if s.smoke else ''}{s.title}")
        print(f"{len(selected)} scenario(s)")
        return 0
    if not selected:
        print(f"no scenario matches {args.k}", file=sys.stderr)
        return 2
    if not (args.azmail_bin.is_file() and os.access(args.azmail_bin, os.X_OK)):
        print(f"azmail binary not found: {args.azmail_bin}\n"
              "build it: cmake --preset mac-debug -S backend && cmake --build backend/build/mac-debug -j\n"
              "or pass --azmail-bin / set AZMAIL_BIN", file=sys.stderr)
        return 2
    args.azmail_bin = args.azmail_bin.resolve()

    ok = True
    setup_failed = False
    try:
        results = run_suite(args, selected, args.blob_backend, f"E2E ({args.blob_backend})")
        setup_failed = all(r.status == "ERROR" and r.detail == "environment setup failed" for r in results)
        ok = summarize(f"E2E ({args.blob_backend})", results)
        smoke = [s for s in selected if s.smoke]
        if args.blob_backend == "r2" and not args.no_local_smoke and not args.k and smoke and not setup_failed:
            smoke_results = run_suite(args, smoke, "local", "local blob store smoke")
            ok = summarize("local blob store smoke", smoke_results) and ok
    except KeyboardInterrupt:
        print("\ninterrupted — processes stopped", file=sys.stderr)
        return 130
    if setup_failed:
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
