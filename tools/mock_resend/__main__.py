"""``python3 -m tools.mock_resend`` runs the mock server (see server.py)."""

import sys

from .server import main

sys.exit(main())
