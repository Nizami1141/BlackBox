"""HTTP client for the *real* AKS described in esp32.html.

Only the fixed endpoints below may receive the device key or a face image.
There is deliberately no local-recognition fallback: an unavailable upstream
must never be mistaken for an approved person.
"""
from __future__ import annotations

from dataclasses import dataclass
from urllib.parse import urlsplit

import httpx


@dataclass
class CentralAksError(Exception):
    status_code: int
    reason: str
    upstream_status: int | None = None


class CentralAksClient:
    def __init__(self, base_url: str, api_key: str, timeout_seconds: float):
        parsed = urlsplit(base_url)
        if (parsed.scheme not in {"http", "https"} or not parsed.hostname
                or parsed.username or parsed.password or parsed.query
                or parsed.fragment):
            raise ValueError("central_aks.base_url must be an HTTP(S) URL without credentials")
        if not api_key:
            raise ValueError("aks_api.api_key is required for central AKS")
        if not 1 <= timeout_seconds <= 120:
            raise ValueError("central_aks.timeout_seconds must be between 1 and 120")
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout_seconds = timeout_seconds

    async def _post(self, path: str, *, data: dict | None = None,
                    files: dict | None = None) -> dict:
        try:
            # No redirects: a redirect must not receive a biometric frame or key.
            # No environment proxy: the private AKS address is reached through
            # the operating system's office-Ethernet route.
            async with httpx.AsyncClient(timeout=self.timeout_seconds,
                                         follow_redirects=False,
                                         trust_env=False) as client:
                response = await client.post(
                    self.base_url + path,
                    headers={"X-API-Key": self.api_key},
                    data=data, files=files,
                )
        except httpx.RequestError as exc:
            raise CentralAksError(502, "CENTRAL_UNREACHABLE") from exc

        if response.status_code != 200:
            reason = {401: "CENTRAL_API_KEY_REJECTED",
                      403: "CENTRAL_IP_NOT_ALLOWED"}.get(
                          response.status_code, "CENTRAL_HTTP_ERROR")
            raise CentralAksError(502, reason, response.status_code)
        try:
            payload = response.json()
        except ValueError as exc:
            raise CentralAksError(502, "CENTRAL_INVALID_JSON") from exc
        if not isinstance(payload, dict) or payload.get("status") != "success":
            raise CentralAksError(502, "CENTRAL_INVALID_RESPONSE")
        return payload

    async def recognize(self, jpeg: bytes) -> dict:
        result = await self._post(
            "/recognize",
            files={"file": ("face.jpg", jpeg, "image/jpeg")},
        )
        faces = result.get("faces")
        if not isinstance(faces, list) or len(faces) != 1 or not isinstance(faces[0], dict):
            raise CentralAksError(502, "CENTRAL_INVALID_FACES")
        face = faces[0]
        if not isinstance(face.get("match"), bool):
            raise CentralAksError(502, "CENTRAL_INVALID_MATCH")
        if face["match"]:
            log_id = face.get("log_id")
            user = face.get("user")
            if (type(log_id) is not int or log_id <= 0
                    or not isinstance(user, dict)
                    or not isinstance(user.get("first_name"), str)
                    or not user["first_name"].strip()
                    or not isinstance(user.get("last_name"), str)
                    or not user["last_name"].strip()):
                raise CentralAksError(502, "CENTRAL_INVALID_MATCH_DATA")
        return result

    async def log_cargo(self, log_id: int, barcode: str) -> dict:
        return await self._post(
            "/log_cargo",
            files={"log_id": (None, str(log_id)),
                   "cargo_barcode": (None, barcode)},
        )
