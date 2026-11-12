"""Configuración del servicio local."""
import os
from pathlib import Path

from dotenv import load_dotenv


ROOT = Path(__file__).resolve().parent


def read_settings() -> dict:
    load_dotenv(ROOT / ".env")
    db_path = Path(os.getenv("EDGE_DB_PATH", "data/safecore-edge.sqlite3"))
    if not db_path.is_absolute():
        db_path = ROOT / db_path
    return {
        "DEVICE_ID": os.getenv("SAFECORE_DEVICE_ID", ""),
        "EDGE_API_KEY": os.getenv("EDGE_DEVICE_API_KEY", ""),
        "DEVICE_SECRET": os.getenv("SAFECORE_DEVICE_SECRET", ""),
        "API_BASE_URL": os.getenv("SAFECORE_API_BASE_URL", "https://backend-safecore.onrender.com").rstrip("/"),
        "DB_PATH": str(db_path),
        "HOST": os.getenv("EDGE_HOST", "127.0.0.1"),
        "PORT": int(os.getenv("EDGE_PORT", "5000")),
        "SYNC_INTERVAL": float(os.getenv("SYNC_INTERVAL_SECONDS", "5")),
    }
