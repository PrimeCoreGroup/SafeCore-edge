"""SQLite compartido entre IAM y telemetría, siguiendo la plantilla."""
from pathlib import Path

from peewee import SqliteDatabase

db = SqliteDatabase(None, timeout=30, pragmas={"journal_mode": "wal", "busy_timeout": 30000})


def configure_database(path: str) -> None:
    target = Path(path).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    if not db.is_closed():
        db.close()
    db.init(str(target))
    db.connect(reuse_if_open=True)
    from iam.infrastructure.models import Device
    from telemetry.infrastructure.models import SensorReading
    db.create_tables([Device, SensorReading], safe=True)
