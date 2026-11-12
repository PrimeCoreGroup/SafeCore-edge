"""Repositorio de lecturas con índice único para reintentos idempotentes."""
from peewee import IntegrityError

from telemetry.infrastructure.models import SensorReading


class ReadingRepository:
    def find(self, device_id: str, event_id: str) -> SensorReading | None:
        return SensorReading.get_or_none(
            SensorReading.device == device_id, SensorReading.event_id == event_id
        )

    def insert(self, device_id: str, event_id: str, fingerprint: str, payload: str) -> tuple[SensorReading, bool]:
        try:
            return SensorReading.create(device=device_id, event_id=event_id,
                                        fingerprint=fingerprint, payload=payload), True
        except IntegrityError:
            return SensorReading.get(SensorReading.device == device_id,
                                     SensorReading.event_id == event_id), False
