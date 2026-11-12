"""Validación del contrato que acepta el backend de SafeCore."""
import hashlib
import json
import math
from datetime import datetime, timedelta, timezone
from uuid import UUID


class ReadingError(ValueError):
    pass


def _number(value, name: str, minimum: float, maximum: float):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ReadingError(f"{name} debe ser un número finito")
    if not minimum <= value <= maximum:
        raise ReadingError(f"{name} debe estar entre {minimum} y {maximum}")
    return value


def normalize(raw: dict, expected_device: str) -> tuple[str, str, str]:
    if not isinstance(raw, dict):
        raise ReadingError("Se requiere un objeto JSON")
    if raw.get("device_id") != expected_device:
        raise ReadingError("device_id no coincide con el dispositivo configurado")
    try:
        UUID(expected_device)
    except (ValueError, AttributeError) as exc:
        raise ReadingError("device_id debe ser UUID") from exc
    event_id = raw.get("event_id")
    if not isinstance(event_id, str) or not 1 <= len(event_id) <= 128 or not event_id.isascii():
        raise ReadingError("event_id debe tener de 1 a 128 caracteres ASCII")
    kind = raw.get("sensor_type")
    if kind not in ("Temperature", "Gas", "Accelerometer"):
        raise ReadingError("sensor_type no admitido")
    source = {"sensorType": kind, "unit": {"Temperature": "C", "Gas": "ADC", "Accelerometer": "raw"}[kind]}
    if kind == "Temperature":
        source["measuredValue"] = _number(raw.get("measured_value"), "measured_value", -127, 125)
    elif kind == "Gas":
        value = _number(raw.get("measured_value"), "measured_value", 0, 4095)
        if int(value) != value:
            raise ReadingError("measured_value de Gas debe ser entero")
        alarm = raw.get("digital_alarm")
        if not isinstance(alarm, bool):
            raise ReadingError("digital_alarm debe ser booleano")
        source.update(measuredValue=int(value), digitalAlarm=alarm)
    else:
        for axis in ("x", "y", "z"):
            value = _number(raw.get("axis_" + axis), "axis_" + axis, -32768, 32767)
            if int(value) != value:
                raise ReadingError("Los ejes deben ser enteros")
            source["axis" + axis.upper()] = int(value)
    captured = raw.get("captured_at")
    if captured is not None:
        if not isinstance(captured, str):
            raise ReadingError("captured_at debe ser ISO 8601 con zona horaria")
        try:
            dt = datetime.fromisoformat(captured.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ReadingError("captured_at inválido") from exc
        if dt.utcoffset() is None:
            raise ReadingError("captured_at necesita zona horaria")
        if dt > datetime.now(timezone.utc) + timedelta(minutes=5):
            raise ReadingError("captured_at no puede superar cinco minutos en el futuro")
        source["capturedAt"] = dt.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    canonical = json.dumps(source, sort_keys=True, separators=(",", ":"))
    fingerprint = hashlib.sha256(canonical.encode()).hexdigest()
    outbound = {"deviceId": expected_device, **source}
    outbound.setdefault("capturedAt", datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"))
    return event_id, fingerprint, json.dumps(outbound, separators=(",", ":"))
