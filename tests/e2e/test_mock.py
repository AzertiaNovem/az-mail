#!/usr/bin/env python3
"""Runs the mock Resend/R2 self-test (Svix + AWS SigV4 vectors, core flows).

    python3 tests/e2e/test_mock.py [-v]      (same as: python3 -m tools.mock_resend.selftest)
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.mock_resend import selftest  # noqa: E402

if __name__ == "__main__":
    sys.exit(selftest.main(sys.argv[1:]))
