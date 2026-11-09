#include <Arduino.h>
#include <ArduinoJson.h>       // ArduinoJson 7
#include <DallasTemperature.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

#if __has_include("config.h")
#include "config.h"
#else
#error Copia config.example.h como config.h y completa Wi-Fi, deviceId y secreto.
#endif
#include "api_ca.h"

// Pines del prototipo físico compartido por el equipo.
constexpr uint8_t PIN_BUZZER = 18;
constexpr uint8_t PIN_LED_VERDE = 23;
constexpr uint8_t PIN_LED_ROJO = 27;
constexpr uint8_t PIN_RELE = 19;       // Módulo de relé activo en LOW.
constexpr uint8_t PIN_MQ_DO = 32;
constexpr uint8_t PIN_MQ_AO = 34;
constexpr uint8_t PIN_DS18B20 = 4;
constexpr uint8_t MPU_ADDR = 0x68;

constexpr float UMBRAL_TEMPERATURA_C = 40.0f;
constexpr int UMBRAL_SUBIDA_GAS_ADC = 100;
constexpr float UMBRAL_IMPACTO_CUENTAS = 200.0f;
constexpr uint32_t VENTANA_IMPACTO_MS = 1500;
constexpr uint32_t VENTANA_GAS_RAPIDO_MS = 1500;
constexpr uint32_t LECTURA_LOCAL_MS = 100;
constexpr uint32_t TEMPERATURA_CADA_MS = 1000;
constexpr uint32_t CONVERSION_TEMPERATURA_MS = 250;  // DS18B20 a 10 bits.
constexpr uint32_t PUBLICACION_CADA_MS = 10000;
constexpr uint32_t HEARTBEAT_CADA_MS = 30000;
constexpr uint32_t REINTENTO_WIFI_MS = 10000;
constexpr uint32_t REINTENTO_HTTP_MS = 3000;

// Cola RAM: mantiene algunas lecturas durante cortes de red, pero no sobrevive
// a un reinicio o corte de energía. Se descarta la más antigua al llenarse.
constexpr uint16_t CAPACIDAD_COLA = 192;
enum class TipoLectura : uint8_t { Temperatura, Gas, Acelerometro };
struct LecturaPendiente {
  uint32_t secuencia;
  uint32_t tomadaEnMs;
  TipoLectura tipo;
  float valor;
  int16_t x, y, z;
  bool alarmaDigital;
};
LecturaPendiente cola[CAPACIDAD_COLA];
uint16_t inicioCola = 0;
uint16_t cantidadCola = 0;
uint32_t siguienteSecuencia = 1;
portMUX_TYPE colaMux = portMUX_INITIALIZER_UNLOCKED;

OneWire oneWire(PIN_DS18B20);
DallasTemperature temperaturaSensor(&oneWire);
float temperaturaC = DEVICE_DISCONNECTED_C;
bool conversionEnCurso = false;
uint32_t conversionIniciadaMs = 0;
uint32_t proximaConversionMs = 0;
float magnitudAnterior = 0;
int gasAnterior = 0;
bool tieneBaseAceleracion = false;
bool tieneBaseGas = false;
bool impactoRegistrado = false;
uint32_t ultimoImpactoMs = 0;
bool gasRapidoRegistrado = false;
uint32_t ultimoGasRapidoMs = 0;
bool alarmaAnterior = false;
uint32_t ultimaLecturaLocalMs = 0;
uint32_t ultimaPublicacionMs = 0;

void encolar(const LecturaPendiente &lectura) {
  portENTER_CRITICAL(&colaMux);
  if (cantidadCola == CAPACIDAD_COLA) {
    inicioCola = (inicioCola + 1) % CAPACIDAD_COLA;
    --cantidadCola;
  }
  LecturaPendiente copia = lectura;
  copia.secuencia = siguienteSecuencia++;
  cola[(inicioCola + cantidadCola) % CAPACIDAD_COLA] = copia;
  ++cantidadCola;
  portEXIT_CRITICAL(&colaMux);
}

bool primeraPendiente(LecturaPendiente &lectura) {
  bool existe;
  portENTER_CRITICAL(&colaMux);
  existe = cantidadCola > 0;
  if (existe) lectura = cola[inicioCola];
  portEXIT_CRITICAL(&colaMux);
  return existe;
}

void confirmarPendiente(uint32_t secuencia) {
  portENTER_CRITICAL(&colaMux);
  if (cantidadCola > 0 && cola[inicioCola].secuencia == secuencia) {
    inicioCola = (inicioCola + 1) % CAPACIDAD_COLA;
    --cantidadCola;
  }
  portEXIT_CRITICAL(&colaMux);
}

bool leerAcelerometro(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(MPU_ADDR, (uint8_t)6, true) != 6) return false;
  x = (int16_t)((Wire.read() << 8) | Wire.read());
  y = (int16_t)((Wire.read() << 8) | Wire.read());
  z = (int16_t)((Wire.read() << 8) | Wire.read());
  return true;
}

void atenderTemperatura(uint32_t ahora) {
  if (conversionEnCurso && ahora - conversionIniciadaMs >= CONVERSION_TEMPERATURA_MS) {
    temperaturaC = temperaturaSensor.getTempCByIndex(0);
    conversionEnCurso = false;
  }
  if (!conversionEnCurso && (int32_t)(ahora - proximaConversionMs) >= 0) {
    temperaturaSensor.requestTemperatures();
    conversionEnCurso = true;
    conversionIniciadaMs = ahora;
    proximaConversionMs = ahora + TEMPERATURA_CADA_MS;
  }
}

void aplicarAlarma(bool alarma) {
  if (alarma == alarmaAnterior) return;
  digitalWrite(PIN_LED_VERDE, alarma ? LOW : HIGH);
  digitalWrite(PIN_LED_ROJO, alarma ? HIGH : LOW);
  digitalWrite(PIN_RELE, alarma ? LOW : HIGH);
  if (alarma) tone(PIN_BUZZER, 2200);
  else noTone(PIN_BUZZER);
  Serial.println(alarma ? "ALERTA LOCAL: salidas activadas" : "Estado normal: salidas desactivadas");
  alarmaAnterior = alarma;
}

void leerYActuar() {
  const uint32_t ahora = millis();
  atenderTemperatura(ahora);
  if (ahora - ultimaLecturaLocalMs < LECTURA_LOCAL_MS) return;
  ultimaLecturaLocalMs = ahora;

  const int gas = analogRead(PIN_MQ_AO);
  const bool gasDigital = digitalRead(PIN_MQ_DO) == LOW;
  if (tieneBaseGas && gas - gasAnterior > UMBRAL_SUBIDA_GAS_ADC) {
    ultimoGasRapidoMs = ahora;
    gasRapidoRegistrado = true;
  }
  const bool gasRapidoActivo = gasRapidoRegistrado && ahora - ultimoGasRapidoMs < VENTANA_GAS_RAPIDO_MS;
  gasAnterior = gas;
  tieneBaseGas = true;

  int16_t x = 0, y = 0, z = 0;
  const bool acelerometroOk = leerAcelerometro(x, y, z);
  if (acelerometroOk) {
    const float magnitud = sqrtf((float)x * x + (float)y * y + (float)z * z);
    if (tieneBaseAceleracion && fabsf(magnitud - magnitudAnterior) > UMBRAL_IMPACTO_CUENTAS) {
      ultimoImpactoMs = ahora;
      impactoRegistrado = true;
    }
    magnitudAnterior = magnitud;
    tieneBaseAceleracion = true;
  }
  const bool impactoActivo = impactoRegistrado && ahora - ultimoImpactoMs < VENTANA_IMPACTO_MS;
  const bool calor = temperaturaC >= UMBRAL_TEMPERATURA_C;
  const bool alarma = gasDigital || gasRapidoActivo || impactoActivo || calor;
  const bool transicion = alarma != alarmaAnterior;
  aplicarAlarma(alarma);

  if (transicion || ahora - ultimaPublicacionMs >= PUBLICACION_CADA_MS) {
    ultimaPublicacionMs = ahora;
    encolar({0, ahora, TipoLectura::Temperatura, temperaturaC, 0, 0, 0, false});
    encolar({0, ahora, TipoLectura::Gas, (float)gas, 0, 0, 0, gasDigital});
    if (acelerometroOk) encolar({0, ahora, TipoLectura::Acelerometro, 0, x, y, z, false});
    Serial.printf("Temp %.1f C | Gas %d ADC | MQ_DO %d | Alarma %d\n", temperaturaC, gas, gasDigital, alarma);
  }
}

bool relojSincronizado() { return time(nullptr) > 1700000000; }

String fechaUtc(uint32_t tomadaEnMs) {
  const uint32_t edadSegundos = (millis() - tomadaEnMs) / 1000;
  const time_t instante = time(nullptr) - edadSegundos;
  struct tm utc;
  gmtime_r(&instante, &utc);
  char texto[25];
  strftime(texto, sizeof(texto), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(texto);
}

int solicitudHttp(const String &ruta, const char *metodo, const String &cuerpo,
                  const String &token, String &respuesta) {
  WiFiClientSecure cliente;
  cliente.setCACert(SAFECORE_API_ROOT_CA);
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3000);
  if (!http.begin(cliente, String(SAFECORE_API_BASE_URL) + ruta)) return -1;
  http.addHeader("Content-Type", "application/json");
  if (token.length()) http.addHeader("Authorization", "Bearer " + token);
  int estado = -1;
  if (strcmp(metodo, "POST") == 0) estado = http.POST(cuerpo);
  else if (strcmp(metodo, "GET") == 0) estado = http.GET();
  if (estado > 0) respuesta = http.getString();
  http.end();
  return estado;
}

bool obtenerToken(String &token, uint32_t &venceEnMs) {
  JsonDocument peticion;
  peticion["deviceId"] = SAFECORE_DEVICE_ID;
  peticion["secret"] = SAFECORE_DEVICE_SECRET;
  String cuerpo, respuesta;
  serializeJson(peticion, cuerpo);
  const int estado = solicitudHttp("/api/iam/auth/device-token", "POST", cuerpo, "", respuesta);
  if (estado != 200) {
    Serial.printf("Autenticacion HTTP %d (401: revisar ID/secreto)\n", estado);
    return false;
  }
  JsonDocument recibido;
  if (deserializeJson(recibido, respuesta)) return false;
  token = recibido["accessToken"].as<String>();
  const uint32_t segundos = recibido["expiresInSeconds"] | 900;
  venceEnMs = millis() + (segundos > 90 ? segundos - 60 : 30) * 1000UL;
  if (token.length() > 0) Serial.println("Token del nodo obtenido");
  return token.length() > 0;
}

int enviarLectura(const LecturaPendiente &lectura, const String &token) {
  JsonDocument peticion;
  peticion["deviceId"] = SAFECORE_DEVICE_ID;
  peticion["capturedAt"] = fechaUtc(lectura.tomadaEnMs);
  switch (lectura.tipo) {
    case TipoLectura::Temperatura:
      peticion["sensorType"] = "Temperature";
      peticion["measuredValue"] = lectura.valor;
      peticion["unit"] = "C";
      break;
    case TipoLectura::Gas:
      peticion["sensorType"] = "Gas";
      peticion["measuredValue"] = (int)lectura.valor;
      peticion["digitalAlarm"] = lectura.alarmaDigital;
      peticion["unit"] = "ADC";
      break;
    case TipoLectura::Acelerometro:
      peticion["sensorType"] = "Accelerometer";
      peticion["axisX"] = lectura.x;
      peticion["axisY"] = lectura.y;
      peticion["axisZ"] = lectura.z;
      peticion["unit"] = "raw";
      break;
  }
  String cuerpo, respuesta;
  serializeJson(peticion, cuerpo);
  const int estado = solicitudHttp("/api/SensorReadings", "POST", cuerpo, token, respuesta);
  if (estado == 201) Serial.println("Lectura enviada (HTTP 201)");
  else Serial.printf("Lectura HTTP %d: %.160s\n", estado, respuesta.c_str());
  return estado;
}

void tareaRed(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  String token;
  uint32_t venceEnMs = 0, ultimoWifiMs = 0, ultimoAuthMs = 0;
  uint32_t ultimoEnvioFallidoMs = 0, ultimoHeartbeatMs = 0;
  bool wifiAnterior = false;
  for (;;) {
    const uint32_t ahora = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiAnterior) Serial.println("Sin Wi-Fi: el control local sigue activo");
      wifiAnterior = false;
      token = "";
      if (ahora - ultimoWifiMs >= REINTENTO_WIFI_MS || ultimoWifiMs == 0) {
        ultimoWifiMs = ahora;
        WiFi.begin(SAFECORE_WIFI_SSID, SAFECORE_WIFI_PASSWORD);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!wifiAnterior) {
      Serial.println("Wi-Fi conectado; sincronizando reloj");
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      wifiAnterior = true;
    }
    if (!relojSincronizado()) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (token.length() == 0 || (int32_t)(ahora - venceEnMs) >= 0) {
      if (ahora - ultimoAuthMs >= REINTENTO_HTTP_MS || ultimoAuthMs == 0) {
        ultimoAuthMs = ahora;
        obtenerToken(token, venceEnMs);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (ahora - ultimoHeartbeatMs >= HEARTBEAT_CADA_MS || ultimoHeartbeatMs == 0) {
      String respuesta;
      const int estado = solicitudHttp(String("/api/devices/") + SAFECORE_DEVICE_ID + "/health",
                                        "POST", "{\"components\":[]}", token, respuesta);
      if (estado == 401) token = "";
      if (estado == 204) Serial.println("Heartbeat confirmado");
      ultimoHeartbeatMs = ahora;
    }
    LecturaPendiente lectura;
    if (primeraPendiente(lectura) &&
        (ultimoEnvioFallidoMs == 0 || ahora - ultimoEnvioFallidoMs >= REINTENTO_HTTP_MS)) {
      const int estado = enviarLectura(lectura, token);
      if (estado == 201) {
        confirmarPendiente(lectura.secuencia);
        ultimoEnvioFallidoMs = 0;
      } else ultimoEnvioFallidoMs = millis();
      if (estado == 401) token = "";
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  digitalWrite(PIN_RELE, HIGH);  // Estado de reposo antes de cambiar el pin a salida.
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_MQ_DO, INPUT);
  analogReadResolution(12);
  digitalWrite(PIN_LED_VERDE, HIGH);
  digitalWrite(PIN_LED_ROJO, LOW);
  noTone(PIN_BUZZER);

  temperaturaSensor.begin();
  temperaturaSensor.setResolution(10);
  temperaturaSensor.setWaitForConversion(false);
  Wire.begin(21, 22);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  if (Wire.endTransmission(true) != 0) Serial.println("MPU6050 no responde; revisar I2C");
  Serial.println("SafeCore Edge: control local activo");
  xTaskCreatePinnedToCore(tareaRed, "SafeCoreRed", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  leerYActuar();
  delay(5);
}
