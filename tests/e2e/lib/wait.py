"""Polling helpers for asynchronous effects (webhooks, jobs, WebSocket events)."""

from __future__ import annotations

import time
from typing import Callable, TypeVar

T = TypeVar("T")


def eventually(fn: Callable[[], T], timeout: float = 15.0, interval: float = 0.25,
               desc: str | None = None) -> T:
    """Calls ``fn`` until it returns without raising AssertionError; returns its result.

    ``fn`` expresses the expectation with asserts (lib.api.ApiError is an AssertionError too).
    On timeout the last AssertionError is re-raised with ``desc`` and the elapsed time.
    """
    deadline = time.monotonic() + timeout
    while True:
        try:
            return fn()
        except AssertionError as exc:
            if time.monotonic() >= deadline:
                what = f"{desc}: " if desc else ""
                raise AssertionError(f"{what}not satisfied within {timeout:.0f}s — last error: {exc}") from exc
        time.sleep(interval)


def wait_for(pred: Callable[[], T], timeout: float = 15.0, interval: float = 0.25,
             desc: str = "condition") -> T:
    """Waits until ``pred()`` returns a truthy value (AssertionErrors count as falsy)."""

    def check() -> T:
        value = pred()
        assert value, f"{desc} is still false"
        return value

    return eventually(check, timeout, interval, desc)


def never(pred: Callable[[], bool], duration: float, interval: float = 0.25,
          desc: str = "condition") -> None:
    """Asserts that ``pred()`` stays falsy for ``duration`` seconds."""
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        if pred():
            raise AssertionError(f"{desc} became true (expected it to stay false for {duration:.0f}s)")
        time.sleep(interval)
