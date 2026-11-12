# SafeCore Edge Service

`SafeCoreEdgeService` es una API local para recibir lecturas del ESP32 del proyecto SafeCore. Está adaptada de la [plantilla Smart Band Edge Service](https://github.com/upc-pre-202620-si572-8725-IoT/smart-band-edge-service-master): mantiene la separación por contextos y capas, la autenticación con `X-API-Key`, la persistencia en SQLite y el identificador `event_id` para reintentos.

El servicio recibe temperatura, gas y aceleración. Guarda cada lectura antes de intentar enviarla al backend de SafeCore. Si Internet falla, las lecturas permanecen en el equipo local y se sincronizan al recuperarse la conexión. El ESP32 controla el LED, el buzzer y el relé de forma independiente del servicio.

**Base del firmware:** `SafeCoreEdgeGateway.ino` parte directamente del sketch original que el equipo usa actualmente en el prototipo. Las únicas adiciones a ese archivo son la inclusión de `EdgeTransport.h`, el arranque de la tarea de red en `setup()` y la copia de las lecturas a la cola en `loop()`. Los pines, lecturas, umbrales, avisos serie y acciones de alarma del sketch original se conservan. El Edge anterior de envío directo no es la base de esta variante.

## Alcance actual

**Implementado**

- Autenticación del ESP32 mediante `device_id` y `X-API-Key`.
- Validación y almacenamiento de lecturas `Temperature`, `Gas` y `Accelerometer`.
- Idempotencia local: un mismo `event_id` con los mismos datos no crea otra lectura.
- Cola persistente en SQLite, reintentos con espera progresiva y estado de entrega.
- Autenticación ante el backend con el secreto del dispositivo, envío de lecturas y heartbeat.
- Firmware que añade envío al gateway al sketch físico original sin cambiar la lógica de las alarmas locales.

**Límites de esta versión**

- El ESP32 conserva temporalmente en RAM hasta 192 lecturas mientras no puede llegar al gateway. Se pierden si se reinicia o se llena la cola.
- El backend actual no admite `event_id`. Una respuesta perdida después de aceptar una lectura podría ocasionar un duplicado en la nube.
- El gateway debe permanecer encendido en la misma red local que el ESP32. La aplicación web no participa en la recepción local.

## ¿Por qué organizarlo por contextos?

La plantilla separa identidad, datos e infraestructura. En SafeCore, esto permite cambiar la validación de un sensor sin modificar cómo se comprueba la clave del ESP32 o cómo se envían las lecturas al backend.

### 1. IAM (identidad y acceso)

Registra el dispositivo local y verifica su clave. El verificador se guarda con sal y PBKDF2; la clave original no se almacena en SQLite. El servicio acepta el UUID configurado para el nodo.

### 2. Telemetría

Valida las lecturas, las convierte al contrato del backend y las guarda. Cada par `device_id` + `event_id` identifica un envío único.

### 3. Sincronización

Un proceso en segundo plano obtiene el token del backend y envía las lecturas pendientes por HTTPS. Las fallas de red se reintentan; una respuesta permanente `4xx` distinta de `401` deja la lectura como `rejected` para revisión.

## Arquitectura por capas

Los contextos `iam` y `telemetry` tienen carpetas `domain`, `application`, `infrastructure` e `interfaces`:

- **Dominio:** reglas de credenciales y de lecturas.
- **Aplicación:** casos de uso de autenticación e ingreso de datos.
- **Infraestructura:** modelos y repositorios Peewee sobre SQLite.
- **Interfaces:** rutas HTTP de Flask.

```text
SafeCoreEdgeService/
├── app.py                  # inicio y composición del servicio
├── config.py               # configuración desde .env
├── iam/                    # identidad del dispositivo local
├── telemetry/              # lecturas e idempotencia
├── sync/                   # integración con el backend SafeCore
├── shared/infrastructure/  # conexión SQLite compartida
├── firmware/SafeCoreEdgeGateway/ # sketch original + transporte añadido
├── tests/
└── requirements.txt
```

Flujo de datos:

```text
Sensores → ESP32 → alarmas locales
                └→ Wi-Fi local → API Flask → SQLite → HTTPS → backend SafeCore → app web
```

## Tecnologías

- Python 3.13, Flask, Peewee, python-dotenv y SQLite.
- Arduino IDE con soporte ESP32, ArduinoJson 7, OneWire y DallasTemperature.
- HTTP con clave local entre ESP32 y gateway; HTTPS con token entre gateway y backend.

Las dependencias Python están en [`requirements.txt`](requirements.txt). La variante del firmware está en [`firmware/SafeCoreEdgeGateway/`](firmware/SafeCoreEdgeGateway/).

## Inicio rápido

Desde esta carpeta, en PowerShell:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
Copy-Item .env.example .env
```

Editar `.env` y colocar el UUID y secreto reales del nodo, una clave local larga y la URL HTTPS del backend. Iniciar:

```powershell
.\.venv\Scripts\python.exe app.py
```

El dispositivo se registra en SQLite al primer inicio. La clave `EDGE_DEVICE_API_KEY` de `.env` debe coincidir con la del firmware. Cambiarla posteriormente en `.env` no rota automáticamente la clave ya guardada. `EDGE_HOST=0.0.0.0` permite acceder desde la red local; el puerto 5000 debe quedar habilitado para esa red.

Para el ESP32, copiar `firmware/SafeCoreEdgeGateway/config.example.h` como `config.h` en la misma carpeta. Configurar Wi-Fi, UUID, clave local e IP privada del gateway en `EDGE_BASE_URL`. El ESP32 **no usa `localhost`** ni necesita el secreto del backend. Abrir `SafeCoreEdgeGateway.ino` en Arduino IDE y cargarlo en la placa. `EdgeTransport.h` aporta la cola RAM y el envío al servicio local; el sketch principal conserva la lógica física original.

La guía de instalación y pruebas con el equipo físico se entrega como PDF aparte; este README documenta el funcionamiento del servicio y su contrato.

## Comportamiento al iniciar

`app.py` crea la base SQLite y sus tablas si aún no existen, registra el nodo configurado, inicia la API local y arranca un trabajador de sincronización. Una lectura nueva pasa a `pending`; después del envío confirmado pasa a `delivered`. Una lectura rechazada de manera permanente pasa a `rejected` y conserva el motivo en SQLite.

Si el ESP32 no incluye `captured_at`, el gateway usa la hora UTC de recepción. Por ello, tras una caída de la red local, la hora registrada puede ser posterior al momento real de medición.

## Contrato de la API local

Todas las rutas requieren el encabezado `X-API-Key` con la clave local.

### Autenticar dispositivo

`POST /api/v1/iam/authentications`

```json
{"device_id":"UUID_DEL_NODO"}
```

Devuelve `200` si la clave es válida y `401` si falta o no coincide.

### Guardar lectura

`POST /api/v1/sensor-readings`

```json
{"device_id":"UUID_DEL_NODO","event_id":"arranque-0001","sensor_type":"Temperature","measured_value":25.5}
```

Campos por sensor:

| `sensor_type` | Datos requeridos | Unidad enviada al backend |
| --- | --- | --- |
| `Temperature` | `measured_value` entre -127 y 125 | `C` |
| `Gas` | `measured_value` entero entre 0 y 4095; `digital_alarm` booleano | `ADC` |
| `Accelerometer` | `axis_x`, `axis_y`, `axis_z` enteros de 16 bits | `raw` |

`captured_at` es opcional y debe tener fecha ISO 8601 con zona horaria. `event_id` debe repetirse sin cambios al reintentar la misma lectura. La primera recepción responde `201`; un reintento idéntico, `200`; el mismo ID con datos diferentes, `409`. Datos inválidos responden `400` y una clave inválida, `401`.

### Consultar estado

`GET /api/v1/status?device_id=UUID_DEL_NODO`

Devuelve las cantidades `pending`, `delivered` y `rejected`. No modifica las lecturas.

## Pruebas

Ejecutar las pruebas de la API y sincronización sin hardware:

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Para la prueba física, verificar las alarmas con y sin Internet, observar `Lectura almacenada en gateway` en el monitor serie y comprobar que las cantidades `pending` y `delivered` cambian según la conexión. La compilación y ejecución en el prototipo aún requieren la placa física.

## Notas operativas

- Ejecutar una sola instancia del gateway por base SQLite y conservar una copia de `data/` antes de reinstalarlo. Las lecturas entregadas permanecen como historial local.
- No compartir `.env` ni `config.h`: ambos contienen credenciales y están excluidos de Git.
- El enlace local usa HTTP dentro de una red de confianza. El envío a la nube verifica el certificado HTTPS.
- La clave local y el secreto del backend son diferentes. El secreto del backend permanece en el gateway.

## Origen y licencia

Este proyecto adapta el repositorio [Smart Band Edge Service](https://github.com/upc-pre-202620-si572-8725-IoT/smart-band-edge-service-master) al caso SafeCore. Se conserva la [licencia MIT de la plantilla](LICENSE.md).
