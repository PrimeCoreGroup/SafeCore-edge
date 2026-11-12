"""Caso de uso de autenticación del dispositivo local."""
from iam.infrastructure.repositories import DeviceRepository


def authenticate(device_id: str, api_key: str, expected_device_id: str) -> bool:
    if device_id != expected_device_id:
        return False
    device = DeviceRepository().find_by_device_id(device_id)
    return device is not None and device.authenticate(api_key)
