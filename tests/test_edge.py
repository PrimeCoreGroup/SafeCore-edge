"""Pruebas de la ingesta local y la sincronización sin hardware ni Internet."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app import create_app
from shared.infrastructure.database import db
from sync.client import CloudError, SafeCoreClient
from sync.service import process_once
from telemetry.infrastructure.models import SensorReading

DEVICE = "12345678-1234-4234-8234-123456789abc"
KEY = "clave-local-de-prueba"


class FakeCloud:
    def __init__(self):
        self.fail = None
        self.received = []

    def send_reading(self, payload):
        if self.fail:
            raise self.fail
        self.received.append(payload)


class FakeHttpResponse:
    def __init__(self, status, payload):
        self.status = status
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self.payload


class EdgeTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parent / "tmp"
        test_root.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=test_root)
        self.settings = {
            "DEVICE_ID": DEVICE, "EDGE_API_KEY": KEY,
            "DB_PATH": str(Path(self.temp.name) / "edge.sqlite3"),
        }
        self.app = create_app(self.settings)
        self.client = self.app.test_client()
        self.headers = {"X-API-Key": KEY}

    def tearDown(self):
        if not db.is_closed():
            db.close()
        self.temp.cleanup()

    def post(self, event="evento-1", value=25.5, headers=None):
        return self.client.post("/api/v1/sensor-readings", headers=self.headers if headers is None else headers,
                                json={"device_id": DEVICE, "event_id": event,
                                      "sensor_type": "Temperature", "measured_value": value})

    def test_auth_idempotence_and_restart(self):
        self.assertEqual(self.post(headers={}).status_code, 401)
        self.assertEqual(self.post().status_code, 201)
        self.assertEqual(self.post().status_code, 200)
        self.assertEqual(self.post(value=26).status_code, 409)
        self.app = create_app(self.settings)
        self.client = self.app.test_client()
        self.assertEqual(self.post().status_code, 200)
        self.assertEqual(SensorReading.select().count(), 1)

    def test_offline_retry_then_delivery(self):
        self.assertEqual(self.post().status_code, 201)
        cloud = FakeCloud()
        cloud.fail = CloudError("sin conexión")
        self.assertTrue(process_once(cloud))
        row = SensorReading.get()
        self.assertEqual(row.status, "pending")
        self.assertEqual(row.attempts, 1)
        row.next_attempt_at = None
        row.save()
        cloud.fail = None
        self.assertTrue(process_once(cloud))
        self.assertEqual(SensorReading.get().status, "delivered")
        self.assertEqual(len(cloud.received), 1)
        self.assertEqual(cloud.received[0]["deviceId"], DEVICE)
        self.assertEqual(cloud.received[0]["sensorType"], "Temperature")
        self.assertIn("capturedAt", cloud.received[0])

    def test_validation_and_permanent_rejection(self):
        invalid = self.client.post("/api/v1/sensor-readings", headers=self.headers,
                                   json={"device_id": DEVICE, "event_id": "gas-1",
                                         "sensor_type": "Gas", "measured_value": 5000,
                                         "digital_alarm": True})
        self.assertEqual(invalid.status_code, 400)
        valid = self.client.post("/api/v1/sensor-readings", headers=self.headers,
                                 json={"device_id": DEVICE, "event_id": "gas-1",
                                       "sensor_type": "Gas", "measured_value": 300,
                                       "digital_alarm": False})
        self.assertEqual(valid.status_code, 201)
        cloud = FakeCloud()
        cloud.fail = CloudError("contrato inválido", 400)
        self.assertTrue(process_once(cloud))
        self.assertEqual(SensorReading.get().status, "rejected")
        self.assertFalse(process_once(cloud))

    def test_accelerometer_payload_and_status_auth(self):
        result = self.client.post("/api/v1/sensor-readings", headers=self.headers,
                                  json={"device_id": DEVICE, "event_id": "accel-1",
                                        "sensor_type": "Accelerometer", "axis_x": 1,
                                        "axis_y": -2, "axis_z": 16384})
        self.assertEqual(result.status_code, 201)
        payload = json.loads(SensorReading.get().payload)
        self.assertEqual((payload["axisX"], payload["axisY"], payload["axisZ"]), (1, -2, 16384))
        path = "/api/v1/status?device_id=" + DEVICE
        self.assertEqual(self.client.get(path).status_code, 401)
        self.assertEqual(self.client.get(path, headers=self.headers).json["pending"], 1)

    def test_cloud_token_and_create_contract(self):
        calls = []

        def reply(request, timeout):
            calls.append((request.full_url, json.loads(request.data), request.headers))
            if request.full_url.endswith("/device-token"):
                return FakeHttpResponse(200, b'{"accessToken":"jwt","expiresInSeconds":900}')
            return FakeHttpResponse(201, b'{"id":"reading-id"}')

        cloud = SafeCoreClient("https://backend.example", DEVICE, "backend-secret")
        with patch("sync.client.urlopen", side_effect=reply):
            cloud.send_reading({"deviceId": DEVICE, "sensorType": "Temperature"})
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][1], {"deviceId": DEVICE, "secret": "backend-secret"})
        self.assertEqual(calls[1][2]["Authorization"], "Bearer jwt")


if __name__ == "__main__":
    unittest.main()
