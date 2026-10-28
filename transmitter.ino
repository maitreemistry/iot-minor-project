#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
#include <string.h>

#include "secrets.h"

// =====================================================
// PIN CONFIGURATION
// =====================================================

#define DHT_PIN 22
#define DHT_TYPE DHT22

#define IR_PIN 5
#define LED_PIN 26

#define MPU_SDA 21
#define MPU_SCL 18

// =====================================================
// SENSOR OBJECTS
// =====================================================

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_MPU6050 mpu;

// =====================================================
// RECEIVER MAC ADDRESS
// =====================================================

uint8_t receiverAddress[] = {
    0x10, 0x06, 0x1C, 0xB6, 0x1D, 0xC4
};

// =====================================================
// THRESHOLDS
// =====================================================

const float TEMP_HIGH = 30.0;

const float HUMIDITY_LOW = 20.0;
const float HUMIDITY_HIGH = 80.0;

const float ACCEL_X_THRESHOLD = 20.0;
const float ACCEL_Y_THRESHOLD = 20.0;
const float ACCEL_Z_THRESHOLD = 20.0;

// =====================================================
// PACKET COUNTERS
// =====================================================

// Counts packets accepted by esp_now_send()
uint32_t packetsSent = 0;

// Counts successful ESP-NOW delivery callbacks
uint32_t packetsDelivered = 0;

// Counts failed ESP-NOW delivery callbacks
uint32_t packetsFailed = 0;

// =====================================================
// ORIGINAL PACKET STRUCTURE
// KEEP THIS STRUCTURE UNCHANGED
// =====================================================

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

// =====================================================
// CALLBACK STATUS
// =====================================================

volatile bool callbackReceived = false;
volatile bool lastDeliverySuccessful = false;

// =====================================================
// MPU6050 CONNECTION CHECK
// =====================================================

bool checkMPU6050Connection() {
    Wire.beginTransmission(0x68);

    if (Wire.endTransmission() == 0) {
        return true;
    }

    Wire.beginTransmission(0x69);

    return Wire.endTransmission() == 0;
}

// =====================================================
// ESP-NOW SEND CALLBACK - ESP32 CORE 3.x
// =====================================================

void OnDataSent(
    const wifi_tx_info_t *info,
    esp_now_send_status_t status
) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        packetsDelivered++;
        lastDeliverySuccessful = true;
    } else {
        packetsFailed++;
        lastDeliverySuccessful = false;
    }

    callbackReceived = true;
}

// =====================================================
// SETUP
// =====================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("       ESP32 SENSOR TRANSMITTER");
    Serial.println("========================================");

    pinMode(IR_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);

    digitalWrite(LED_PIN, LOW);

    // Initialize sensors
    dht.begin();

    Wire.begin(MPU_SDA, MPU_SCL);

    if (mpu.begin()) {
        Serial.println("[INIT] MPU6050 initialized");
    } else {
        Serial.println("[WARN] MPU6050 not detected");
    }

    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    Serial.print("[INFO] Packet structure size: ");
    Serial.print(sizeof(sensorData));
    Serial.println(" bytes");

    if (sizeof(sensorData) != 40) {
        Serial.println(
            "[WARN] Structure size differs from 40 bytes"
        );
    }

    // Initialize Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[INIT] Connecting to Wi-Fi");

    unsigned long startTime = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - startTime < 15000
    ) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[OK] Wi-Fi connected");
    } else {
        Serial.println("[WARN] Wi-Fi connection timed out");
    }

    Serial.print("[INFO] Transmitter MAC: ");
    Serial.println(WiFi.macAddress());

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERROR] ESP-NOW initialization failed");
        return;
    }

    esp_now_register_send_cb(OnDataSent);

    // Configure receiver
    esp_now_peer_info_t peerInfo = {};

    memcpy(
        peerInfo.peer_addr,
        receiverAddress,
        6
    );

    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[ERROR] Failed to add receiver");
        return;
    }

    Serial.println("[OK] ESP-NOW initialized");
    Serial.println("[OK] Receiver peer configured");

    Serial.println("----------------------------------------");
    Serial.println("System ready");
    Serial.println("========================================");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
    // -------------------------------------------------
    // 1. READ TEMPERATURE AND HUMIDITY
    // -------------------------------------------------

    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    sensorData.temperatureConnected = !isnan(temperature);
    sensorData.humidityConnected = !isnan(humidity);

    sensorData.temperature =
        sensorData.temperatureConnected ? temperature : 0.0f;

    sensorData.humidity =
        sensorData.humidityConnected ? humidity : 0.0f;

    // -------------------------------------------------
    // 2. READ IR SENSOR
    // -------------------------------------------------

    sensorData.irValue = digitalRead(IR_PIN);

    // A digital GPIO input cannot reliably identify
    // physical disconnection of the IR sensor.
    sensorData.irSensorConnected = true;

    // -------------------------------------------------
    // 3. READ MPU6050
    // -------------------------------------------------

    sensorData.accelerometerConnected =
        checkMPU6050Connection();

    if (sensorData.accelerometerConnected) {
        sensors_event_t a, g, temp;

        if (mpu.getEvent(&a, &g, &temp)) {
            sensorData.ax = a.acceleration.x;
            sensorData.ay = a.acceleration.y;
            sensorData.az = a.acceleration.z;

            // Monitoring only; not used for threshold checks.
            sensorData.acceleration = sqrtf(
                sensorData.ax * sensorData.ax +
                sensorData.ay * sensorData.ay +
                sensorData.az * sensorData.az
            );
        } else {
            sensorData.accelerometerConnected = false;

            sensorData.ax = 0.0f;
            sensorData.ay = 0.0f;
            sensorData.az = 0.0f;
            sensorData.acceleration = 0.0f;
        }
    } else {
        sensorData.ax = 0.0f;
        sensorData.ay = 0.0f;
        sensorData.az = 0.0f;
        sensorData.acceleration = 0.0f;
    }

    // -------------------------------------------------
    // 4. TEMPERATURE ALERT
    // -------------------------------------------------

    sensorData.temperatureAlert =
        !sensorData.temperatureConnected ||
        sensorData.temperature > TEMP_HIGH;

    // -------------------------------------------------
    // 5. HUMIDITY ALERT
    // -------------------------------------------------

    sensorData.humidityAlert =
        !sensorData.humidityConnected ||
        sensorData.humidity < HUMIDITY_LOW ||
        sensorData.humidity > HUMIDITY_HIGH;

    // -------------------------------------------------
    // 6. IR ALERT
    // -------------------------------------------------

    sensorData.irOffAlert =
        !sensorData.irSensorConnected ||
        sensorData.irValue == HIGH;

    // -------------------------------------------------
    // 7. INDIVIDUAL AXIS ALERTS
    // -------------------------------------------------

    bool xAxisAlert =
        fabsf(sensorData.ax) > ACCEL_X_THRESHOLD;

    bool yAxisAlert =
        fabsf(sensorData.ay) > ACCEL_Y_THRESHOLD;

    bool zAxisAlert =
        fabsf(sensorData.az) > ACCEL_Z_THRESHOLD;

    sensorData.accelerationAlert =
        !sensorData.accelerometerConnected ||
        xAxisAlert ||
        yAxisAlert ||
        zAxisAlert;

    // -------------------------------------------------
    // 8. OVERALL ALERT
    // -------------------------------------------------

    sensorData.alertActive =
        sensorData.temperatureAlert ||
        sensorData.humidityAlert ||
        sensorData.irOffAlert ||
        sensorData.accelerationAlert;

    // -------------------------------------------------
    // 9. LED
    // -------------------------------------------------

    digitalWrite(
        LED_PIN,
        sensorData.alertActive ? HIGH : LOW
    );

    // -------------------------------------------------
    // 10. ORGANIZED SERIAL MONITOR OUTPUT
    // -------------------------------------------------

    Serial.println();
    Serial.println("========================================");
    Serial.printf("        SENSOR REPORT | Uptime: %lu ms\n",
                  (unsigned long)millis());
    Serial.println("========================================");

    // Temperature and humidity
    Serial.println("[1] ENVIRONMENT");

    Serial.printf(
        "Temperature : %s",
        sensorData.temperatureConnected ? "" : "DISCONNECTED "
    );

    if (sensorData.temperatureConnected) {
        Serial.printf("%.2f C", sensorData.temperature);
    }
    Serial.println();

    Serial.printf(
        "Humidity    : %s",
        sensorData.humidityConnected ? "" : "DISCONNECTED "
    );

    if (sensorData.humidityConnected) {
        Serial.printf("%.2f %%", sensorData.humidity);
    }
    Serial.println();

    Serial.printf(
        "Temp Alert  : %s\n",
        sensorData.temperatureAlert ? "ACTIVE" : "NORMAL"
    );

    Serial.printf(
        "Humidity Alert: %s\n",
        sensorData.humidityAlert ? "ACTIVE" : "NORMAL"
    );

    // IR sensor
    Serial.println("----------------------------------------");
    Serial.println("[2] IR SENSOR");

    Serial.printf("Digital Value: %d\n", sensorData.irValue);

    Serial.printf(
        "IR Alert     : %s\n",
        sensorData.irOffAlert ? "ACTIVE" : "NORMAL"
    );

    // Accelerometer
    Serial.println("----------------------------------------");
    Serial.println("[3] MPU6050 ACCELERATION");

    Serial.printf(
        "Sensor Status: %s\n",
        sensorData.accelerometerConnected
            ? "CONNECTED"
            : "DISCONNECTED"
    );

    Serial.printf(
        "X: %7.2f m/s^2 | Limit: %.2f | %s\n",
        sensorData.ax,
        ACCEL_X_THRESHOLD,
        xAxisAlert ? "ALERT" : "NORMAL"
    );

    Serial.printf(
        "Y: %7.2f m/s^2 | Limit: %.2f | %s\n",
        sensorData.ay,
        ACCEL_Y_THRESHOLD,
        yAxisAlert ? "ALERT" : "NORMAL"
    );

    Serial.printf(
        "Z: %7.2f m/s^2 | Limit: %.2f | %s\n",
        sensorData.az,
        ACCEL_Z_THRESHOLD,
        zAxisAlert ? "ALERT" : "NORMAL"
    );

    Serial.printf(
        "Magnitude (monitoring): %.2f m/s^2\n",
        sensorData.acceleration
    );

    Serial.printf(
        "Acceleration Alert: %s\n",
        sensorData.accelerationAlert ? "ACTIVE" : "NORMAL"
    );

    // Overall system status
    Serial.println("----------------------------------------");
    Serial.println("[4] SYSTEM STATUS");

    Serial.printf(
        "Overall Alert: %s\n",
        sensorData.alertActive ? "!!! ACTIVE !!!" : "NORMAL"
    );

    Serial.printf(
        "LED Status   : %s\n",
        sensorData.alertActive ? "ON" : "OFF"
    );

    // -------------------------------------------------
    // 11. TRANSMIT PACKET
    // -------------------------------------------------

    callbackReceived = false;

    esp_err_t result = esp_now_send(
        receiverAddress,
        (uint8_t *)&sensorData,
        sizeof(sensorData)
    );

    if (result == ESP_OK) {
        packetsSent++;

        Serial.println("----------------------------------------");
        Serial.println("[5] ESP-NOW TRANSMISSION");
        Serial.println("Send request accepted");

        Serial.printf(
            "Packet Size        : %u bytes\n",
            (unsigned int)sizeof(sensorData)
        );

        Serial.printf(
            "Packets Submitted  : %lu\n",
            (unsigned long)packetsSent
        );

        // Wait briefly for the asynchronous callback.
        // The callback updates delivery counters.
        unsigned long callbackStart = millis();

        while (
            !callbackReceived &&
            millis() - callbackStart < 200
        ) {
            delay(1);
        }

        if (callbackReceived) {
            Serial.printf(
                "Latest Delivery    : %s\n",
                lastDeliverySuccessful ? "SUCCESS" : "FAILED"
            );
        } else {
            Serial.println(
                "Latest Delivery    : CALLBACK PENDING"
            );
        }
    } else {
        Serial.println("----------------------------------------");
        Serial.println("[5] ESP-NOW TRANSMISSION");
        Serial.printf("Send request error: %d\n", result);
    }

    Serial.println("----------------------------------------");
    Serial.println("[6] PACKET STATISTICS");

    Serial.printf(
        "Submitted : %lu\n",
        (unsigned long)packetsSent
    );

    Serial.printf(
        "Delivered : %lu\n",
        (unsigned long)packetsDelivered
    );

    Serial.printf(
        "Failed    : %lu\n",
        (unsigned long)packetsFailed
    );

    Serial.println("========================================");

    delay(1000);
}


