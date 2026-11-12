"""Interfaz HTTP de telemetría."""
from flask import Blueprint, current_app, jsonify, request
from peewee import fn

from iam.application.service import authenticate
from telemetry.application.service import receive
from telemetry.domain.reading import ReadingError
from telemetry.infrastructure.models import SensorReading

blueprint = Blueprint("telemetry", __name__)


def authorized(device_id: str) -> bool:
    return authenticate(device_id, request.headers.get("X-API-Key", ""),
                        current_app.config["EDGE_SETTINGS"]["DEVICE_ID"])


@blueprint.post("/api/v1/sensor-readings")
def ingest():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify(error="Se requiere un objeto JSON"), 400
    device_id = data.get("device_id")
    if not isinstance(device_id, str) or not authorized(device_id):
        return jsonify(error="Credenciales inválidas"), 401
    try:
        status, result = receive(data, device_id)
    except ReadingError as exc:
        return jsonify(error=str(exc)), 400
    return jsonify(result), status


@blueprint.get("/api/v1/status")
def status():
    device_id = request.args.get("device_id", "")
    if not authorized(device_id):
        return jsonify(error="Credenciales inválidas"), 401
    counts = {r.status: r.count for r in SensorReading.select(
        SensorReading.status, fn.COUNT(SensorReading.id).alias("count")
    ).group_by(SensorReading.status)}
    return jsonify(device_id=device_id, pending=counts.get("pending", 0),
                   delivered=counts.get("delivered", 0), rejected=counts.get("rejected", 0))
