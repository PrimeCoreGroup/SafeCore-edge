# SafeCore Edge

Firmware para el nodo ESP32 del prototipo físico de SafeCore. Lee los sensores y controla la alarma en el propio dispositivo; cuando hay Internet, envía telemetría al [backend de SafeCore](https://github.com/PrimeCoreGroup/SafeCore-backend).

## Cómo funciona

1. El ESP32 lee el MQ-2 y el MPU6050 cada 100 ms y el DS18B20 aproximadamente cada segundo.
2. Activa el LED rojo, el buzzer y el relé cuando detecta temperatura de 40 °C o más, alarma digital del MQ-2, una subida rápida de más de 100 cuentas ADC de gas o una variación de más de 200 cuentas en la magnitud de aceleración. En reposo mantiene encendido el LED verde.
3. La lectura y la actuación se ejecutan localmente. Una tarea de red separada intenta conectar el Wi-Fi sin detener la alarma cuando no hay Internet.
4. Con conexión, obtiene un token del nodo, sincroniza el reloj por NTP y envía lecturas de temperatura, gas y aceleración por HTTPS. Publica una tanda cada 10 segundos y otra cuando cambia el estado de alarma. También reporta el estado del nodo cada 30 segundos.
5. Durante una desconexión guarda hasta 192 lecturas pendientes en RAM y las envía al reconectar. Si la cola se llena, descarta las más antiguas.

El backend almacena las lecturas y evalúa los riesgos. La aplicación web permite registrar el nodo y consultar sus datos; no interviene en la decisión local de activar la alarma.

## Hardware del prototipo

| Componente | Pin ESP32 |
| --- | --- |
| Buzzer | GPIO18 |
| LED verde | GPIO23 |
| LED rojo | GPIO27 |
| Relé activo en LOW | GPIO19 |
| MQ-2 digital | GPIO32 |
| MQ-2 analógico | GPIO34 |
| DS18B20 | GPIO4 |
| MPU6050 SDA / SCL | GPIO21 / GPIO22 |

El MPU6050 utiliza la dirección I²C `0x68`. La entrada analógica del ESP32 debe recibir como máximo 3,3 V; comprueba que la salida del módulo MQ-2 esté acondicionada antes de conectarla.

## Archivos

```text
SafeCoreEdge/
├── SafeCoreEdge.ino      Lectura, alarma local y comunicación HTTPS
├── api_ca.h              Certificado raíz público para verificar HTTPS
├── config.example.h      Plantilla de Wi-Fi y credenciales del nodo
└── .gitignore            Excluye config.h
```

## Cargar en el ESP32

1. Instala el core **esp32 by Espressif Systems** en Arduino IDE y las bibliotecas **ArduinoJson 7**, **OneWire** y **DallasTemperature**.
2. Registra un nodo en SafeCore y copia su `deviceId` y secreto de un solo uso. La aplicación web muestra el secreto al crearlo; si se pierde, hay que rotarlo.
3. Copia `SafeCoreEdge/config.example.h` como `SafeCoreEdge/config.h` y completa el Wi-Fi, `deviceId` y secreto. `config.h` está excluido de Git.
4. Abre `SafeCoreEdge/SafeCoreEdge.ino`, selecciona la placa y el puerto, verifica y sube el sketch. El Monitor Serie usa **115200 baudios**.

La URL de la API se define en `config.h`; la plantilla apunta al despliegue actual en Render. La conexión HTTPS verifica el certificado del servidor mediante `api_ca.h`.

## Alcance actual

- La cola offline está solo en RAM: se pierde al reiniciar o cortar la alimentación. El funcionamiento ante un corte eléctrico depende de la alimentación del prototipo.
- Los umbrales se definen en el sketch; este firmware no descarga cambios desde la aplicación web.
- No consulta los comandos de prueba de actuadores pendientes en el backend ni confirma su ejecución.
- No implementa MQTT ni notificaciones desde el ESP32.
- La compilación y las pruebas con el montaje físico están pendientes de realizar por el integrante que tiene el dispositivo.
