"""Cola durable: reintenta cortes y conserva errores permanentes para inspección."""
import json
import logging
import time
from datetime import datetime, timedelta, timezone

from shared.infrastructure.database import db
from sync.client import CloudError, SafeCoreClient
from telemetry.infrastructure.models import SensorReading

log = logging.getLogger(__name__)


def process_once(client: SafeCoreClient) -> bool:
    """Procesa una lectura elegible; True indica que hubo trabajo."""
    with db.connection_context():
        now = datetime.now(timezone.utc)
        row = (SensorReading.select()
               .where((SensorReading.status == "pending") &
                      ((SensorReading.next_attempt_at.is_null(True)) |
                       (SensorReading.next_attempt_at <= now)))
               .order_by(SensorReading.id).first())
        if row is None:
            return False
        try:
            client.send_reading(json.loads(row.payload))
        except CloudError as exc:
            row.attempts += 1
            row.last_error = str(exc)[:500]
            if exc.status is not None and 400 <= exc.status < 500 and exc.status != 401:
                row.status = "rejected"
                row.next_attempt_at = None
                log.error("Lectura %s rechazada: %s", row.event_id, exc)
            else:
                delay = min(300, 5 * 2 ** min(row.attempts - 1, 6))
                row.next_attempt_at = now + timedelta(seconds=delay)
                log.warning("Lectura %s pendiente: %s", row.event_id, exc)
        else:
            row.status = "delivered"
            row.delivered_at = datetime.now(timezone.utc)
            row.next_attempt_at = None
            row.last_error = None
            log.info("Lectura %s entregada", row.event_id)
        row.save()
        return True


def run_worker(settings: dict) -> None:
    client = SafeCoreClient(settings["API_BASE_URL"], settings["DEVICE_ID"],
                            settings.get("DEVICE_SECRET", ""))
    last_heartbeat = 0.0
    while True:
        try:
            if process_once(client):
                continue
            if time.monotonic() - last_heartbeat >= 30 and settings.get("DEVICE_SECRET"):
                last_heartbeat = time.monotonic()
                try:
                    client.heartbeat()
                except CloudError as exc:
                    log.warning("Heartbeat pendiente: %s", exc)
        except Exception:
            log.exception("Error inesperado en sincronización")
        time.sleep(settings.get("SYNC_INTERVAL", 5))
