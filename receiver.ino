#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "emqx_ca.h"

// =====================================================
// MQTT CONFIGURATION
// =====================================================

const char mqtt_server[] = "broker.emqx.io";
const int mqtt_port = 8883;

const char* sensorTopic =
    "emqx/esp32/iot_project";

const char* commandTopic =
    "emqx/esp32/iot_project/command";

const char* responseTopic =
    "emqx/esp32/iot_project/response";

const char* alertTopic =
    "emqx/esp32/iot_project/alert";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// =====================================================
// ESP-NOW STRUCTURE
// MUST MATCH THE TRANSMITTER EXACTLY
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

static_assert(
    sizeof(struct_message) == 40,
    "ERROR: ESP-NOW packet structure must be 40 bytes"
);

// =====================================================
// CONFIGURATION
// =====================================================

const uint32_t EXPECTED_PACKET_INTERVAL_MS = 1200;
const uint32_t PACKET_GAP_THRESHOLD_MS = 2400;
const uint32_t TRANSMITTER_TIMEOUT_MS = 6000;

const int ALARM_PIN = 25;

// =====================================================
// GLOBAL VARIABLES
// =====================================================

struct_message sensorData = {};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool newDataReceived = false;
volatile bool packetLossDetected = false;

volatile bool packetSizeMismatchDetected = false;
volatile bool packetSizeAlertPending = false;

volatile uint32_t receivedPacketSize = 0;

volatile uint32_t receivedPackets = 0;
volatile uint32_t invalidPackets = 0;
volatile uint32_t estimatedMissedPackets = 0;
volatile uint32_t missedPacketsOnLatestReception = 0;
volatile uint32_t lastPacketGapMs = 0;
volatile uint32_t lastPacketTime = 0;

bool transmitterConnected = false;
bool publishingEnabled = true;

uint32_t lastReconnectAttempt = 0;
uint32_t lastTimeoutCheck = 0;

String alertMessage = "No active alerts";

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

void connectWiFi();
void initializeESPNow();
void reconnectMQTT();

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
);

void OnDataRecv(
    const esp_now_recv_info_t* info,
    const uint8_t* incomingData,
    int len
);

String createSensorReport();
String createAlertReport();

void printSensorReport();
void publishSensorReport();
void publishAlertReport();

void updateAlertMessage();
void updateBuzzer();

// =====================================================
// BUZZER CONTROL
// =====================================================

void updateBuzzer() {
    struct_message data;

    bool connected;
    bool lossDetected;
    bool sizeMismatch;

    portENTER_CRITICAL(&mux);

    data = sensorData;
    connected = transmitterConnected;
    lossDetected = packetLossDetected;
    sizeMismatch = packetSizeMismatchDetected;

    portEXIT_CRITICAL(&mux);

    bool alarmActive =
        data.alertActive ||
        lossDetected ||
        sizeMismatch ||
        !connected;

    digitalWrite(
        ALARM_PIN,
        alarmActive ? HIGH : LOW
    );
}

// =====================================================
// WIFI CONNECTION
// =====================================================

void connectWiFi() {
    Serial.println();
    Serial.println("Connecting to Wi-Fi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Wi-Fi connected.");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Wi-Fi Channel: ");
    Serial.println(WiFi.channel());
}

// =====================================================
// ESP-NOW INITIALIZATION
// =====================================================

void initializeESPNow() {
    Serial.println();
    Serial.println("Initializing ESP-NOW...");

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed.");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);

    Serial.println("ESP-NOW initialized successfully.");
    Serial.println("Waiting for transmitter data...");
}

// =====================================================
// ESP-NOW RECEIVE CALLBACK
// =====================================================

void OnDataRecv(
    const esp_now_recv_info_t* info,
    const uint8_t* incomingData,
    int len
) {
    // -------------------------------------------------
    // PACKET VALIDATION
    // -------------------------------------------------

    if (incomingData == nullptr ||
        len != sizeof(struct_message)) {

        portENTER_CRITICAL(&mux);

        invalidPackets++;

        receivedPacketSize =
            incomingData == nullptr ? 0 : (uint32_t)len;

        packetSizeMismatchDetected = true;
        packetSizeAlertPending = true;

        portEXIT_CRITICAL(&mux);

        return;
    }

    // -------------------------------------------------
    // VALID PACKET
    // -------------------------------------------------

    struct_message receivedData;

    memcpy(
        &receivedData,
        incomingData,
        sizeof(receivedData)
    );

    uint32_t currentTime = millis();

    portENTER_CRITICAL(&mux);

    uint32_t gap = 0;
    uint32_t missedNow = 0;

    if (lastPacketTime != 0) {
        gap = currentTime - lastPacketTime;

        if (gap > PACKET_GAP_THRESHOLD_MS) {
            uint32_t intervals =
                gap / EXPECTED_PACKET_INTERVAL_MS;

            if (intervals > 1) {
                missedNow = intervals - 1;
            }
        }
    }

    lastPacketTime = currentTime;

    sensorData = receivedData;

    receivedPackets++;

    receivedPacketSize = sizeof(struct_message);

    lastPacketGapMs = gap;

    missedPacketsOnLatestReception = missedNow;

    estimatedMissedPackets += missedNow;

    packetLossDetected = (missedNow > 0);

    // A correctly sized packet has been received.
    packetSizeMismatchDetected = false;

    transmitterConnected = true;

    newDataReceived = true;

    portEXIT_CRITICAL(&mux);
}

// =====================================================
// UPDATE ALERT MESSAGE
// =====================================================

void updateAlertMessage() {
    struct_message data;

    bool connected;
    bool lossDetected;
    bool sizeMismatch;

    portENTER_CRITICAL(&mux);

    data = sensorData;

    connected = transmitterConnected;
    lossDetected = packetLossDetected;
    sizeMismatch = packetSizeMismatchDetected;

    portEXIT_CRITICAL(&mux);

    String message = "";

    if (!connected) {
        message += "Transmitter disconnected; ";
    }

    if (sizeMismatch) {
        message += "ESP-NOW packet size mismatch; ";
    }

    if (!data.temperatureConnected) {
        message += "Temperature sensor disconnected; ";
    } else if (data.temperatureAlert) {
        message += "High temperature; ";
    }

    if (!data.humidityConnected) {
        message += "Humidity sensor disconnected; ";
    } else if (data.humidityAlert) {
        message += "Humidity out of range; ";
    }

    if (!data.irSensorConnected) {
        message += "IR sensor status unavailable; ";
    } else if (data.irOffAlert) {
        message += "IR sensor alert; ";
    }

    if (!data.accelerometerConnected) {
        message += "Accelerometer disconnected; ";
    } else if (data.accelerationAlert) {
        message += "Acceleration threshold exceeded; ";
    }

    if (lossDetected) {
        message += "Possible packet loss detected; ";
    }

    if (message.isEmpty()) {
        message = "No active alerts";
    } else {
        message.remove(message.length() - 2);
    }

    alertMessage = message;
}

// =====================================================
// CREATE FORMATTED SENSOR REPORT
// Used by Serial Monitor and MQTTX
// =====================================================

String createSensorReport() {
    struct_message data;

    uint32_t rxCount;
    uint32_t invalidCount;
    uint32_t missedCount;
    uint32_t missedLatest;
    uint32_t packetGap;
    uint32_t actualPacketSize;

    bool connected;
    bool lossDetected;
    bool sizeMismatch;

    portENTER_CRITICAL(&mux);

    data = sensorData;

    rxCount = receivedPackets;
    invalidCount = invalidPackets;
    missedCount = estimatedMissedPackets;
    missedLatest = missedPacketsOnLatestReception;
    packetGap = lastPacketGapMs;

    actualPacketSize = receivedPacketSize;

    connected = transmitterConnected;
    lossDetected = packetLossDetected;
    sizeMismatch = packetSizeMismatchDetected;

    portEXIT_CRITICAL(&mux);

    String report;
    report.reserve(1500);

    // -------------------------------------------------
    // [1] ENVIRONMENT
    // -------------------------------------------------

    report += "[1] ENVIRONMENT\n";

    report += "Temperature      : ";

    if (data.temperatureConnected) {
        report += String(data.temperature, 2);
        report += " C\n";
    } else {
        report += "DISCONNECTED\n";
    }

    report += "Humidity         : ";

    if (data.humidityConnected) {
        report += String(data.humidity, 2);
        report += " %\n";
    } else {
        report += "DISCONNECTED\n";
    }

    report += "Temperature Alert: ";
    report += data.temperatureAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "Humidity Alert   : ";
    report += data.humidityAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    // -------------------------------------------------
    // [2] IR SENSOR
    // -------------------------------------------------

    report += "[2] IR SENSOR\n";

    report += "Digital Value    : ";
    report += String(data.irValue);
    report += "\n";

    report += "Sensor Status    : ";

    if (data.irSensorConnected) {
        report += "CONNECTED\n";
    } else {
        report += "DISCONNECTED\n";
    }

    report += "IR Alert         : ";
    report += data.irOffAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    // -------------------------------------------------
    // [3] MPU6050 ACCELERATION
    // -------------------------------------------------

    report += "[3] MPU6050 ACCELERATION\n";

    report += "Sensor Status    : ";

    if (data.accelerometerConnected) {
        report += "CONNECTED\n";
    } else {
        report += "DISCONNECTED\n";
    }

    report += "X: ";
    report += String(data.ax, 2);
    report += " m/s^2\n";

    report += "Y: ";
    report += String(data.ay, 2);
    report += " m/s^2\n";

    report += "Z: ";
    report += String(data.az, 2);
    report += " m/s^2\n";

    report += "Magnitude        : ";
    report += String(data.acceleration, 2);
    report += " m/s^2\n";

    report += "Acceleration Alert: ";
    report += data.accelerationAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    // -------------------------------------------------
    // [4] ESP-NOW TRANSMISSION
    // -------------------------------------------------

    report += "[4] ESP-NOW TRANSMISSION\n";

    report += "Transmitter      : ";
    report += connected ? "CONNECTED\n" : "DISCONNECTED\n";

    report += "Expected Size    : ";
    report += String(sizeof(struct_message));
    report += " bytes\n";

    report += "Received Size    : ";
    report += String(actualPacketSize);
    report += " bytes\n";

    report += "Packet Size Check: ";
    report += sizeMismatch ? "MISMATCH DETECTED\n" : "NORMAL\n";

    report += "Latest Reception : ";
    report += connected ? "ACTIVE\n" : "TIMEOUT\n";

    report += "Last Packet Gap  : ";
    report += String(packetGap);
    report += " ms\n";

    report += "------------------------------------------\n";

    // -------------------------------------------------
    // [5] PACKET STATISTICS
    // -------------------------------------------------

    report += "[5] PACKET STATISTICS\n";

    report += "Received         : ";
    report += String(rxCount);
    report += "\n";

    report += "Invalid          : ";
    report += String(invalidCount);
    report += "\n";

    report += "Estimated Missed : ";
    report += String(missedCount);
    report += "\n";

    report += "Missed Latest    : ";
    report += String(missedLatest);
    report += "\n";

    report += "Packet Loss      : ";
    report += lossDetected ? "POSSIBLE\n" : "NOT DETECTED\n";

    report += "==========================================\n";

    return report;
}

// =====================================================
// CREATE FORMATTED ALERT REPORT
// Dedicated MQTT alert topic
// =====================================================

String createAlertReport() {
    updateAlertMessage();

    struct_message data;

    bool connected;
    bool lossDetected;
    bool sizeMismatch;

    portENTER_CRITICAL(&mux);

    data = sensorData;

    connected = transmitterConnected;
    lossDetected = packetLossDetected;
    sizeMismatch = packetSizeMismatchDetected;

    portEXIT_CRITICAL(&mux);

    bool overallAlert =
        data.alertActive ||
        lossDetected ||
        sizeMismatch ||
        !connected;

    String report;
    report.reserve(800);

    report += "[1] ENVIRONMENT ALERTS\n";

    report += "Temperature Alert : ";
    report += data.temperatureAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "Humidity Alert    : ";
    report += data.humidityAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    report += "[2] IR SENSOR ALERT\n";

    report += "IR Alert          : ";
    report += data.irOffAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    report += "[3] ACCELERATION ALERT\n";

    report += "Acceleration Alert: ";
    report += data.accelerationAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    report += "[4] COMMUNICATION ALERTS\n";

    report += "Transmitter       : ";
    report += connected ? "CONNECTED\n" : "DISCONNECTED\n";

    report += "Packet Loss       : ";
    report += lossDetected ? "POSSIBLE\n" : "NOT DETECTED\n";

    report += "Packet Size       : ";
    report += sizeMismatch ? "MISMATCH DETECTED\n" : "NORMAL\n";

    report += "------------------------------------------\n";

    report += "[5] SYSTEM STATUS\n";

    report += "Overall Alert     : ";
    report += overallAlert ? "ACTIVE\n" : "NORMAL\n";

    report += "Alert Message     : ";
    report += alertMessage;
    report += "\n";

    report += "==========================================\n";

    return report;
}

// =====================================================
// PRINT SENSOR REPORT TO SERIAL MONITOR
// =====================================================

void printSensorReport() {
    String report = createSensorReport();

    Serial.println();
    Serial.println(report);
}

// =====================================================
// PUBLISH SENSOR REPORT TO MQTTX
// Topic: emqx/esp32/iot_project
// =====================================================

void publishSensorReport() {
    if (!client.connected() || !publishingEnabled) {
        return;
    }

    String report = createSensorReport();

    bool success = client.publish(
        sensorTopic,
        report.c_str()
    );

    if (success) {
        Serial.println("Sensor report published to MQTT.");
    } else {
        Serial.println("Sensor MQTT publishing failed.");
    }
}

// =====================================================
// PUBLISH ALERT REPORT TO MQTTX
// Topic: emqx/esp32/iot_project/alert
// =====================================================

void publishAlertReport() {
    if (!client.connected()) {
        return;
    }

    String report = createAlertReport();

    bool success = client.publish(
        alertTopic,
        report.c_str(),
        true
    );

    if (success) {
        Serial.println("Alert report published to MQTT.");
    } else {
        Serial.println("Alert MQTT publishing failed.");
    }
}

// =====================================================
// MQTT COMMAND CALLBACK
// =====================================================

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
) {
    String message = "";

    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    message.trim();
    message.toLowerCase();

    if (String(topic) != commandTopic) {
        return;
    }

    String response;

    if (message == "hello") {
        response = "hii";
    }
    else if (message == "start" || message == "resume") {
        publishingEnabled = true;
        response = "Sensor publishing started";
    }
    else if (message == "pause" || message == "stop") {
        publishingEnabled = false;
        response = "Sensor publishing paused";
    }
    else {
        response = "Unknown command: " + message;
    }

    client.publish(
        responseTopic,
        response.c_str()
    );

    Serial.println();
    Serial.println("========== MQTT COMMAND ==========");
    Serial.print("Command: ");
    Serial.println(message);
    Serial.print("Response: ");
    Serial.println(response);
    Serial.println("==================================");
}

// =====================================================
// MQTT RECONNECTION
// =====================================================

void reconnectMQTT() {
    if (WiFi.status() != WL_CONNECTED ||
        client.connected()) {
        return;
    }

    uint32_t currentTime = millis();

    if (lastReconnectAttempt != 0 &&
        currentTime - lastReconnectAttempt < 5000) {
        return;
    }

    lastReconnectAttempt = currentTime;

    Serial.println("Connecting to MQTT broker...");

    String clientId = "ESP32_Receiver_";

    clientId += String(
        (uint32_t)ESP.getEfuseMac(),
        HEX
    );

    if (client.connect(clientId.c_str())) {
        Serial.println("MQTT connected successfully.");

        client.subscribe(commandTopic);

        client.publish(
            responseTopic,
            "ESP32 receiver connected"
        );

        client.setBufferSize(2048);

        if (lastPacketTime != 0) {
            publishSensorReport();
            publishAlertReport();
        }
    } else {
        Serial.print("MQTT connection failed. State: ");
        Serial.println(client.state());
    }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
    pinMode(ALARM_PIN, OUTPUT);
    digitalWrite(ALARM_PIN, LOW);

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("       ESP32 ESP-NOW RECEIVER");
    Serial.println("==========================================");

    Serial.print("Expected ESP-NOW packet size: ");
    Serial.print(sizeof(struct_message));
    Serial.println(" bytes");

    connectWiFi();

    // Verify the broker certificate using EMQX's official CA certificate.
    // Save emqx_ca.h beside this sketch and paste the downloaded PEM
    // certificate into EMQX_CA_CERT (see the accompanying header template).
    espClient.setCACert(EMQX_CA_CERT);

    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
    client.setBufferSize(2048);

    initializeESPNow();

    reconnectMQTT();

    Serial.println();
    Serial.println("Receiver setup complete.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
    // -------------------------------------------------
    // MAINTAIN MQTT CONNECTION
    // -------------------------------------------------

    if (WiFi.status() == WL_CONNECTED) {
        if (!client.connected()) {
            reconnectMQTT();
        } else {
            client.loop();
        }
    } else {
        WiFi.reconnect();
    }

    uint32_t currentTime = millis();

    // -------------------------------------------------
    // TRANSMITTER TIMEOUT DETECTION
    // -------------------------------------------------

    if (currentTime - lastTimeoutCheck >= 500) {
        lastTimeoutCheck = currentTime;

        uint32_t lastTime;

        portENTER_CRITICAL(&mux);
        lastTime = lastPacketTime;
        portEXIT_CRITICAL(&mux);

        bool previouslyConnected = transmitterConnected;

        if (lastTime != 0 &&
            currentTime - lastTime > TRANSMITTER_TIMEOUT_MS) {
            transmitterConnected = false;
        }

        if (previouslyConnected && !transmitterConnected) {
            Serial.println();
            Serial.println("Transmitter disconnected.");

            updateAlertMessage();
            publishAlertReport();
            updateBuzzer();
        }
    }

    // -------------------------------------------------
    // PROCESS PACKET-SIZE MISMATCH ALERTS
    // -------------------------------------------------

    bool packetSizeAlertOccurred = false;

    portENTER_CRITICAL(&mux);

    if (packetSizeAlertPending) {
        packetSizeAlertPending = false;
        packetSizeAlertOccurred = true;
    }

    portEXIT_CRITICAL(&mux);

    if (packetSizeAlertOccurred) {
        Serial.println();
        Serial.println("ESP-NOW packet size mismatch detected.");

        Serial.print("Expected packet size: ");
        Serial.print(sizeof(struct_message));
        Serial.println(" bytes");

        Serial.print("Received packet size: ");
        Serial.print(receivedPacketSize);
        Serial.println(" bytes");

        updateAlertMessage();

        // Display the latest sensor and packet report.
        printSensorReport();

        // Publish the receiver-generated mismatch alert.
        publishAlertReport();

        updateBuzzer();
    }

    // -------------------------------------------------
    // PROCESS NEW SENSOR DATA
    // -------------------------------------------------

    bool dataAvailable = false;

    portENTER_CRITICAL(&mux);

    if (newDataReceived) {
        newDataReceived = false;
        dataAvailable = true;
    }

    portEXIT_CRITICAL(&mux);

    if (dataAvailable) {
        updateAlertMessage();

        // Display the complete formatted report in Serial.
        printSensorReport();

        // Publish readings and statistics on the main topic.
        if (publishingEnabled) {
            publishSensorReport();
        }

        // Publish alerts only on the dedicated alert topic.
        publishAlertReport();
    }

    updateBuzzer();

    delay(10);
}
