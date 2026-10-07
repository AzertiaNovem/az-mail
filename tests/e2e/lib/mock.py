"""Client for the mock's control endpoints (/_mock/*, see tools/mock_resend/README.md)."""

from __future__ import annotations

import base64
from typing import Any, Iterable

from .api import Api, http_request


class MockClient:
    def __init__(self, base: str, api_key: str, s3_base: str | None = None) -> None:
        self.base = base.rstrip("/")
        self.api_key = api_key
        self.s3_base = s3_base
        self._api = Api(self.base)

    # -- control --------------------------------------------------------------------------------
    def _get(self, endpoint: str, query: dict[str, Any] | None = None) -> Any:
        return self._api.get(endpoint, query=query or None)

    def _post(self, endpoint: str, body: Any = None, query: dict[str, Any] | None = None) -> Any:
        return self._api.post(endpoint, body if body is not None else {}, query=query or None)

    def state(self) -> dict[str, Any]:
        return self._get("/_mock/state")

    def seq(self) -> int:
        return int(self.state()["seq"])

    def config(self, **values: Any) -> dict[str, Any]:
        return self._post("/_mock/config", values) if values else self._get("/_mock/config")

    def faults(self, faults: list[dict[str, Any]] | None = None, append: bool = False) -> list[dict[str, Any]]:
        if faults is None:
            return self._get("/_mock/faults")["faults"]
        body: Any = {"faults": faults, "append": True} if append else faults
        return self._post("/_mock/faults", body)["faults"]

    def clear_faults(self) -> None:
        self._post("/_mock/faults", [])

    def soft_reset(self) -> None:
        """Config, faults, clock offset and violations back to the start values; data kept."""
        self._post("/_mock/reset", {"keep_data": True})

    def advance(self, seconds: float) -> dict[str, Any]:
        return self._post("/_mock/advance", None, {"seconds": seconds})

    def sent(self, since: int = 0) -> dict[str, Any]:
        return self._get("/_mock/sent", {"since": since})

    def requests(self, since: int = 0, method: str | None = None, path: str | None = None) -> list[dict[str, Any]]:
        return self._get("/_mock/sent", {"since": since, "method": method, "path": path})["requests"]

    def emails(self, since: int = 0, subject: str | None = None) -> list[dict[str, Any]]:
        out = self._get("/_mock/sent", {"since": since})["emails"]
        return [e for e in out if subject is None or e["subject"] == subject]

    def email(self, email_id: str) -> dict[str, Any]:
        return self._get(f"/_mock/emails/{email_id}")

    def posts(self, since: int = 0, subject: str | None = None,
              idempotency_key: str | None = None) -> list[dict[str, Any]]:
        """POST /emails requests (incl. faulted and replayed ones), optionally filtered."""
        out = []
        for r in self.requests(since, method="POST", path="/emails"):
            body = r.get("body") or {}
            if subject is not None and body.get("subject") != subject:
                continue
            if idempotency_key is not None and r.get("idempotency_key") != idempotency_key:
                continue
            out.append(r)
        return out

    def received(self, since: int = 0) -> list[dict[str, Any]]:
        return self._get("/_mock/received", {"since": since})["received"]

    def received_raw(self, received_id: str) -> bytes:
        resp = http_request("GET", f"{self.base}/_mock/received/{received_id}/raw")
        assert resp.status == 200, resp.short()
        return resp.body

    def inbound(self, *, from_: str, to: Iterable[str] | str = (), attachments: list[dict[str, Any]] | None = None,
                **fields: Any) -> dict[str, Any]:
        """Injects external mail (POST /_mock/inbound). Attachment ``data`` (bytes) is base64'd."""
        body: dict[str, Any] = {"from": from_, "to": [to] if isinstance(to, str) else list(to)}
        atts = []
        for a in attachments or []:
            a = dict(a)
            if "data" in a:
                a["content"] = base64.b64encode(a.pop("data")).decode()
            atts.append(a)
        if atts:
            body["attachments"] = atts
        body.update(fields)
        return self._post("/_mock/inbound", body)

    def webhook(self, **body: Any) -> dict[str, Any]:
        return self._post("/_mock/webhook", body)

    def webhooks(self, since: int = 0, wait: bool = False) -> dict[str, Any]:
        return self._get("/_mock/webhooks", {"since": since, "wait": "1" if wait else None})

    def violations(self) -> list[dict[str, Any]]:
        return self._get("/_mock/violations")["violations"]

    def s3(self, since: int = 0) -> dict[str, Any]:
        return self._get("/_mock/s3", {"since": since})

    def s3_object(self, sha256: str) -> dict[str, Any] | None:
        for obj in self.s3()["objects"]:
            if obj["key"].endswith("/" + sha256):
                return obj
        return None
