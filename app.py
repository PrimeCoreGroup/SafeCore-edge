"""API local de SafeCore Edge Service, adaptada de Smart Band Edge Service."""
import logging
import threading

from flask import Flask

from config import read_settings
from iam.interfaces.api import blueprint as iam_blueprint
from iam.infrastructure.repositories import DeviceRepository
from shared.infrastructure.database import configure_database, db
from telemetry.interfaces.api import blueprint as telemetry_blueprint


def create_app(settings: dict | None = None) -> Flask:
    config = read_settings() if settings is None else settings
    if not config.get("DEVICE_ID") or not config.get("EDGE_API_KEY"):
        raise ValueError("Configura SAFECORE_DEVICE_ID y EDGE_DEVICE_API_KEY")
    configure_database(config["DB_PATH"])
    # Igual que la plantilla: registrar el dispositivo de desarrollo una vez.
    # Una clave nueva en .env no sustituye la almacenada; se debe migrar/rotar explícitamente.
    DeviceRepository().get_or_create_test_device(config["DEVICE_ID"], config["EDGE_API_KEY"])

    app = Flask(__name__)
    app.config["EDGE_SETTINGS"] = config
    app.config["MAX_CONTENT_LENGTH"] = 4096
    app.register_blueprint(iam_blueprint)
    app.register_blueprint(telemetry_blueprint)

    @app.teardown_appcontext
    def close_db(_error):
        if not db.is_closed():
            db.close()

    return app


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    settings = read_settings()
    app = create_app(settings)
    from sync.service import run_worker
    thread = threading.Thread(target=run_worker, args=(settings,), daemon=True, name="safecore-sync")
    thread.start()
    app.run(host=settings["HOST"], port=settings["PORT"], debug=False, use_reloader=False, threaded=True)


if __name__ == "__main__":
    main()
