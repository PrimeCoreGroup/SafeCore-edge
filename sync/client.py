"""Cliente HTTPS del backend SafeCore; mantiene el secreto fuera del firmware."""
import json
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


class CloudError(Exception):
    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


class SafeCoreClient:
    def __init__(self, base_url: str, device_id: str, secret: str):
        if not base_url.startswith("https://"):
            raise ValueError("SAFECORE_API_BASE_URL debe usar HTTPS")
        self.base_url = base_url.rstrip("/")
        self.device_id = device_id
        self.secret = secret
        self.token = ""
        self.token_expires = 0.0

    def _post(self, path: str, body: dict, token: str = "") -> tuple[int, dict | None]:
        payload = json.dumps(body).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        req = Request(self.base_url + path, data=payload, headers=headers, method="POST")
        try:
            with urlopen(req, timeout=6) as response:
                content = response.read()
                return response.status, json.loads(content) if content else None
        except HTTPError as exc:
            detail = exc.read(200).decode("utf-8", errors="replace")
            raise CloudError(f"HTTP {exc.code}: {detail}", exc.code) from exc
        except (URLError, TimeoutError, OSError) as exc:
            raise CloudError(str(exc)) from exc

    def authenticate(self) -> str:
        if not self.secret:
            raise CloudError("Falta SAFECORE_DEVICE_SECRET")
        status, response = self._post("/api/iam/auth/device-token",
                                       {"deviceId": self.device_id, "secret": self.secret})
        if status != 200 or not isinstance(response, dict) or not response.get("accessToken"):
            raise CloudError("Respuesta de autenticación inválida")
        self.token = response["accessToken"]
        self.token_expires = time.monotonic() + max(10, int(response.get("expiresInSeconds", 900)) - 60)
        return self.token

    def _valid_token(self) -> str:
        return self.token if self.token and time.monotonic() < self.token_expires else self.authenticate()

    def send_reading(self, payload: dict) -> None:
        try:
            status, _ = self._post("/api/SensorReadings", payload, self._valid_token())
        except CloudError as exc:
            if exc.status == 401:
                self.token = ""
            raise
        if status != 201:
            raise CloudError(f"Respuesta inesperada del backend: {status}")

    def heartbeat(self) -> None:
        try:
            status, _ = self._post(f"/api/devices/{self.device_id}/health",
                                    {"components": []}, self._valid_token())
        except CloudError as exc:
            if exc.status == 401:
                self.token = ""
            raise
        if status != 204:
            raise CloudError(f"Heartbeat inesperado: {status}")
