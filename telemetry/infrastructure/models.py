"""Lecturas persistidas y estado de entrega al backend."""
from datetime import datetime, timezone

from peewee import AutoField, CharField, DateTimeField, ForeignKeyField, IntegerField, Model, TextField

from iam.infrastructure.models import Device
from shared.infrastructure.database import db


class SensorReading(Model):
    id = AutoField()
    device = ForeignKeyField(Device, backref="readings", column_name="device_id")
    event_id = CharField(max_length=128)
    fingerprint = CharField(max_length=64)
    payload = TextField()
    status = CharField(default="pending")
    attempts = IntegerField(default=0)
    next_attempt_at = DateTimeField(null=True)
    last_error = TextField(null=True)
    received_at = DateTimeField(default=lambda: datetime.now(timezone.utc))
    delivered_at = DateTimeField(null=True)

    class Meta:
        database = db
        table_name = "sensor_readings"
        indexes = ((('device', 'event_id'), True),)
