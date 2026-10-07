"""Mock Resend API + minimal Cloudflare R2 (S3) endpoint for AZ Mail tests and development.

Standard library only (Python >= 3.10). Entry points:
  python3 -m tools.mock_resend           # same as tools.mock_resend.server
  python3 -m tools.mock_resend.selftest  # self-test (Svix + SigV4 vectors, core flows)
"""

__version__ = "1.0.0"
