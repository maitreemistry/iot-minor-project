
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
#include <string.h>

#include "secrets.h"  // NEW: Separate Wi-Fi credentials

// ==================================================
// PIN DEFINITIONS
// ==================================================

#define DHTPIN 22
#define DHTTYPE DHT22
#define IRPIN 5
#define LEDPIN 26

// ==================================================
// RECEIVER MAC ADDRESS
// ==================================================

uint8_t receiverAddress[] = {
  0x10, 0x06, 0x1C, 0xB6, 0x1D, 0xC4
};

// ==================================================
// ALERT THRESHOLDS
// ==================================================

const float TEMP_HIGH = 40.0;
const float HUMIDITY_LOW = 20.0;
const float HUMIDITY_HIGH = 80.0;
const float ACCEL_THRESHOLD = 8.0;

// ==================================================
// SENSOR OBJECTS
// ==================================================

DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;

bool mpuConnected = false;

// ==================================================
// DATA STRUCTURE
// MUST MATCH RECEIVER EXACTLY
// ==================================================

typedef struct struct_message {
  float temperature;
  float humidity;

  int irValue;

  float ax;
  float ay;
  float az;
  float acceleration;

  bool temperatureConnected;
  bool humidityConnected;
  bool irSensorConnected;
  bool accelerometerConnected;

  bool temperatureAlert;
  bool humidityAlert;
  bool irOffAlert;
  bool accelerationAlert;

  bool alertActive;
} struct_message;

struct_message sensorData;

const size_t EXPECTED_PACKET_LENGTH = sizeof(struct_message);

// ==================================================
// ESP-NOW SEND CALLBACK
// ESP32 ARDUINO CORE 3.x
// ==================================================

void OnDataSent(
  const wifi_tx_info_t* info,
  esp_now_send_status_t status
) {
  Serial.print("ESP-NOW delivery status: ");

  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("SUCCESS");
  } else {
    Serial.println("FAIL");
  }
}

// ==================================================
// SETUP
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========== TRANSMITTER STARTING ==========");

  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, LOW);

  pinMode(IRPIN, INPUT_PULLUP);

  dht.begin();

  // MPU6050: SDA = GPIO 21, SCL = GPIO 18
  Wire.begin(21, 18);
  mpuConnected = mpu.begin();

  if (mpuConnected) {
    mpu.setFilterBandwidth(MPU6050_BAND_94_HZ);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    Serial.println("MPU6050 connected successfully.");
  } else {
    Serial.println("WARNING: MPU6050 not detected.");
  }

  // NEW: Read credentials from secrets.h
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < 20000
  ) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection failed!");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("\nWi-Fi connected.");

  Serial.print("Transmitter MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("Wi-Fi channel: ");
  Serial.println(WiFi.channel());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed!");

    while (true) {
      delay(1000);
    }
  }

  esp_now_register_send_cb(OnDataSent);

  // Register receiver as peer
  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, receiverAddress, 6);

  // Use the current Wi-Fi channel
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add receiver peer!");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("ESP-NOW initialized successfully.");
  Serial.println("MQ135 removed from this project.");

  Serial.printf(
    "Fixed ESP-NOW payload length: %u bytes\n",
    (unsigned int)EXPECTED_PACKET_LENGTH
  );

  Serial.println("Transmitter ready.");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected. Reconnecting...");
    WiFi.reconnect();
    delay(1000);
    return;
  }

  // 1. READ DHT22
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  sensorData.temperatureConnected = !isnan(t);
  sensorData.humidityConnected = !isnan(h);

  if (sensorData.temperatureConnected) {
    sensorData.temperature = t;
  }

  if (sensorData.humidityConnected) {
    sensorData.humidity = h;
  }

  // 2. READ IR SENSOR
  sensorData.irValue = digitalRead(IRPIN);
  sensorData.irSensorConnected = true;

  // Assumption: HIGH = no object / IR off
  //             LOW  = object detected

  // 3. READ MPU6050
  sensorData.accelerometerConnected = mpuConnected;

  float accelerationMagnitude = 0.0f;

  if (mpuConnected) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    sensorData.ax = a.acceleration.x;
    sensorData.ay = a.acceleration.y;
    sensorData.az = a.acceleration.z;

    accelerationMagnitude = sqrtf(
      a.acceleration.x * a.acceleration.x +
      a.acceleration.y * a.acceleration.y +
      a.acceleration.z * a.acceleration.z
    );

    sensorData.acceleration = accelerationMagnitude;
  } else {
    sensorData.ax = 0.0f;
    sensorData.ay = 0.0f;
    sensorData.az = 0.0f;
    sensorData.acceleration = 0.0f;
  }

  // 4. CALCULATE ALERTS
  sensorData.temperatureAlert =
    sensorData.temperatureConnected &&
    sensorData.temperature > TEMP_HIGH;

  sensorData.humidityAlert =
    sensorData.humidityConnected &&
    (
      sensorData.humidity < HUMIDITY_LOW ||
      sensorData.humidity > HUMIDITY_HIGH
    );

  sensorData.irOffAlert =
    sensorData.irSensorConnected &&
    sensorData.irValue == HIGH;

  sensorData.accelerationAlert =
    sensorData.accelerometerConnected &&
    accelerationMagnitude > ACCEL_THRESHOLD;

  sensorData.alertActive =
    sensorData.temperatureAlert ||
    sensorData.humidityAlert ||
    sensorData.irOffAlert ||
    sensorData.accelerationAlert;

  // 5. CONTROL LED
  digitalWrite(
    LEDPIN,
    sensorData.alertActive ? HIGH : LOW
  );

  // 6. PRINT SENSOR READINGS
  Serial.println();
  Serial.println("========== SENSOR READINGS ==========");

  if (sensorData.temperatureConnected) {
    Serial.printf(
      "Temperature: %.2f C\n",
      sensorData.temperature
    );
  } else {
    Serial.println("Temperature: SENSOR ERROR");
  }

  if (sensorData.humidityConnected) {
    Serial.printf(
      "Humidity: %.2f %%\n",
      sensorData.humidity
    );
  } else {
    Serial.println("Humidity: SENSOR ERROR");
  }

  Serial.printf(
    "IR status: %s\n",
    sensorData.irValue == LOW
      ? "OBJECT DETECTED"
      : "NO OBJECT / IR OFF"
  );

  if (sensorData.accelerometerConnected) {
    Serial.printf(
      "Acceleration X: %.3f m/s^2\n",
      sensorData.ax
    );
    Serial.printf(
      "Acceleration Y: %.3f m/s^2\n",
      sensorData.ay
    );
    Serial.printf(
      "Acceleration Z: %.3f m/s^2\n",
      sensorData.az
    );
    Serial.printf(
      "Acceleration magnitude: %.3f m/s^2\n",
      sensorData.acceleration
    );
  } else {
    Serial.println("MPU6050: NOT CONNECTED");
  }

  // 7. PRINT ALERT STATUS
  Serial.println("------------ ALERT STATUS ------------");

  Serial.printf(
    "Temperature alert: %s\n",
    sensorData.temperatureAlert ? "ACTIVE" : "NORMAL"
  );
  Serial.printf(
    "Humidity alert: %s\n",
    sensorData.humidityAlert ? "ACTIVE" : "NORMAL"
  );
  Serial.printf(
    "IR off alert: %s\n",
    sensorData.irOffAlert ? "ACTIVE" : "NORMAL"
  );
  Serial.printf(
    "Acceleration alert: %s\n",
    sensorData.accelerationAlert ? "ACTIVE" : "NORMAL"
  );
  Serial.printf(
    "Overall alert: %s\n",
    sensorData.alertActive ? "ACTIVE" : "NORMAL"
  );
  Serial.printf(
    "LED status: %s\n",
    sensorData.alertActive ? "ON" : "OFF"
  );

  // 8. VALIDATE AND SEND FIXED-LENGTH PAYLOAD
  const size_t packetLength = sizeof(sensorData);

  Serial.printf(
    "Transmitted payload length: %u bytes (expected: %u bytes)\n",
    (unsigned int)packetLength,
    (unsigned int)EXPECTED_PACKET_LENGTH
  );

  if (packetLength != EXPECTED_PACKET_LENGTH) {
    Serial.println(
      "SECURITY ALERT: Local payload length mismatch. Packet NOT sent."
    );
  } else {
    Serial.printf(
      "TEST: Sending %u bytes; expected %u bytes\n",
      (unsigned int)packetLength,
      (unsigned int)EXPECTED_PACKET_LENGTH
    );

    esp_err_t result = esp_now_send(
      receiverAddress,
      (const uint8_t*)&sensorData,
      packetLength
    );

    if (result == ESP_OK) {
      Serial.println("Test packet submitted successfully.");
    } else {
      Serial.printf("Send error: %d\n", result);
    }
  }

  Serial.println("======================================");
  delay(2000);
}
