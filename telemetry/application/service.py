"""Caso de uso de ingesta con idempotencia durable."""
from telemetry.domain.reading import normalize
from telemetry.infrastructure.models import SensorReading
from telemetry.infrastructure.repositories import ReadingRepository


def receive(raw: dict, device_id: str) -> tuple[int, dict]:
    event_id, fingerprint, payload = normalize(raw, device_id)
    repo = ReadingRepository()
    existing = repo.find(device_id, event_id)
    if existing is not None:
        return _replay(existing, fingerprint)
    row, created = repo.insert(device_id, event_id, fingerprint, payload)
    if not created:
        return _replay(row, fingerprint)
    return 201, {"event_id": event_id, "status": row.status}


def _replay(row: SensorReading, fingerprint: str) -> tuple[int, dict]:
    if row.fingerprint != fingerprint:
        return 409, {"error": "event_id ya existe con una lectura diferente"}
    return 200, {"event_id": row.event_id, "status": row.status}
