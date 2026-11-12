"""Interfaz HTTP del contexto IAM."""
from flask import Blueprint, current_app, jsonify, request

from iam.application.service import authenticate

blueprint = Blueprint("iam", __name__)


@blueprint.post("/api/v1/iam/authentications")
def authentication():
    data = request.get_json(silent=True) or {}
    device_id = data.get("device_id")
    if not isinstance(device_id, str) or not authenticate(
        device_id, request.headers.get("X-API-Key", ""), current_app.config["EDGE_SETTINGS"]["DEVICE_ID"]
    ):
        return jsonify(error="Credenciales inválidas"), 401
    return jsonify(device_id=device_id, authenticated=True), 200
