#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "EdgeTransport.h"

// --- CONFIGURACIÓN DE PINES ---
const int MPU_addr = 0x68; 
const int BUZZER_PIN = 18;  // Buzzer en D18
const int LED_VERDE = 23;   // LED Verde en D23 (Sistema listo)
const int LED_ROJO = 27;    // LED Rojo en D27 (Alerta)
const int RELAY_PIN = 19;   // Relé en D19 (Cable negro en IN)

// --- PINES DEL MQ-2 ---
const int MQ_DO = 32;       // Pin digital del MQ-2 (Cable amarillo)
const int MQ_AO = 34;       // Pin analógico del MQ-2 (Cable verde)

// --- SENSOR DE TEMPERATURA DS18B20 ---
const int ONE_WIRE_BUS = 4;   // Pin de datos del DS18B20 en D4 (Cable amarillo/señal)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

int16_t AcX, AcY, AcZ;
float magnitudAnterior = 0;
int gasAnterior = 0;
bool primeraLectura = true;

// Temporizador para mantener la alerta unos segundos tras un impacto brusco (el golpe es un instante)
unsigned long tiempoUltimoImpacto = 0;
const unsigned long DURACION_ALERTA_IMPACTO = 1500; // 1.5 segundos

void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(MQ_DO, INPUT);
  
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_ROJO, LOW);
  
  // Relé en reposo (abierto / válvula sin bloquear)
  digitalWrite(RELAY_PIN, HIGH); 
  
  // Iniciar sensor de temperatura DS18B20
  sensors.begin();

  // Iniciar MPU6050
  Wire.begin(21, 22);
  Wire.beginTransmission(MPU_addr);
  Wire.write(0x6B); 
  Wire.write(0);    
  Wire.endTransmission(true);
  
  Serial.println("SafeCore Iniciando sistema completo (Movimiento + Gas + Calor)...");
  delay(1000); 
  
  digitalWrite(LED_VERDE, HIGH);
  tone(BUZZER_PIN, 3000, 100); // Tono de inicio
  
  Serial.println("\n>>> ¡SISTEMA ACTIVO! Monitoreando temperatura, gases e impactos.");
  edgeBegin(); // Añadido: inicia el envío al gateway en otra tarea del ESP32.
}

void loop() {
  // 1. Leer Temperatura del DS18B20
  sensors.requestTemperatures(); 
  float temperaturaC = sensors.getTempCByIndex(0);

  // 2. Leer el sensor de gas MQ-2
  int gasActual = analogRead(MQ_AO);
  int estadoDigitalMQ = digitalRead(MQ_DO); // LOW si detecta gas

  // Imprimir lecturas en el Monitor Serie para control
  Serial.print("Temp: "); Serial.print(temperaturaC); Serial.print(" °C | ");
  Serial.print("Gas AO: "); Serial.print(gasActual);
  Serial.print(" | Gas DO: "); Serial.println(estadoDigitalMQ);

  if (primeraLectura) {
    Wire.beginTransmission(MPU_addr);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_addr, 6, true);
    AcX = Wire.read() << 8 | Wire.read();
    AcY = Wire.read() << 8 | Wire.read();
    AcZ = Wire.read() << 8 | Wire.read();
    magnitudAnterior = sqrt((long)AcX * AcX + (long)AcY * AcY + (long)AcZ * AcZ);
    gasAnterior = gasActual;
    primeraLectura = false;
    delay(100);
    return;
  }

  int diferenciaGas = gasActual - gasAnterior;
  const int UMBRAL_SUBIDA_GAS = 100; 

  // 3. Leer el acelerómetro (MPU6050)
  Wire.beginTransmission(MPU_addr);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_addr, 6, true);
  
  AcX = Wire.read() << 8 | Wire.read();
  AcY = Wire.read() << 8 | Wire.read();
  AcZ = Wire.read() << 8 | Wire.read();

  float magnitudActual = sqrt((long)AcX * AcX + (long)AcY * AcY + (long)AcZ * AcZ);
  float diferenciaAceleracion = abs(magnitudActual - magnitudAnterior);
  const float UMBRAL_IMPACTO = 200.0; 

  if (diferenciaAceleracion > UMBRAL_IMPACTO) {
    tiempoUltimoImpacto = millis();
    Serial.println("¡Impacto / Sismo detectado!");
  }

  // 4. Evaluar condiciones de peligro (O Gas, O Calor, O Movimiento)
  bool hayGas = (estadoDigitalMQ == LOW) || (diferenciaGas > UMBRAL_SUBIDA_GAS);
  bool hayImpactoActivo = (millis() - tiempoUltimoImpacto < DURACION_ALERTA_IMPACTO);
  
  // Umbral de temperatura para detectar calor excesivo / fuego (puedes ajustarlo si deseas)
  const float UMBRAL_CALOR = 40.0; 
  bool hayCalorExcesivo = (temperaturaC >= UMBRAL_CALOR);

  if (hayGas || hayImpactoActivo || hayCalorExcesivo) {
    // --- SI HAY ALGUN PELIGRO: Alarma total, LED rojo y relé cerrado (clic) ---
    if (hayGas) Serial.println("🔥 ALERTA: ¡Gas detectado!");
    if (hayImpactoActivo) Serial.println("💥 ALERTA: ¡Movimiento o impacto detectado!");
    if (hayCalorExcesivo) Serial.println("🌡️ ALERTA: ¡Exceso de calor / fuego detectado!");

    digitalWrite(LED_VERDE, LOW);  
    digitalWrite(LED_ROJO, HIGH);  
    digitalWrite(RELAY_PIN, LOW);  // Relé activado (clic de válvula cerrada)
    
    tone(BUZZER_PIN, 2200);        // La bocina suena continuamente mientras persista el peligro
  } else {
    // --- CUANDO TODO ESTÁ SEGURO: Se apaga solo y el relé vuelve a abrirse ---
    digitalWrite(LED_ROJO, LOW);   
    digitalWrite(LED_VERDE, HIGH); 
    digitalWrite(RELAY_PIN, HIGH); // Relé desactivado (clic de apertura normal)
    noTone(BUZZER_PIN);
  }

  // Añadido: copia las lecturas a una cola sin detener el control local.
  edgePublish(temperaturaC, gasActual, estadoDigitalMQ == LOW,
              AcX, AcY, AcZ, hayGas || hayImpactoActivo || hayCalorExcesivo);

  magnitudAnterior = magnitudActual;
  gasAnterior = gasActual;
  
  delay(100);
}
