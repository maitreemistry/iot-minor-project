#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h"  // NEW: Wi-Fi credentials stored separately

// ==================================================
// WIFI SETTINGS
// ==================================================
// Credentials are defined in secrets.h

// ==================================================
// MQTT SETTINGS
// ==================================================

const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 8883;

const char* sensorTopic =
  "emqx/esp32/iot_project";

const char* commandTopic =
  "emqx/esp32/iot_project/command";

const char* responseTopic =
  "emqx/esp32/iot_project/response";

const char* alertTopic =
  "emqx/esp32/iot_project/alert";

// ==================================================
// MQTT OBJECTS - TLS
// ==================================================

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ==================================================
// DATA STRUCTURE
// MUST MATCH TRANSMITTER EXACTLY
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

const size_t EXPECTED_PACKET_LENGTH =
  sizeof(struct_message);

// ==================================================
// ESP-NOW DATA HANDLING
// ==================================================

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool newDataReceived = false;

// Invalid packet-size alert state
volatile bool invalidPacketSizeReceived = false;
volatile int invalidReceivedLength = 0;

unsigned long lastPacketTime = 0;

const unsigned long TRANSMITTER_TIMEOUT = 6000;

bool transmitterConnected = false;
bool previousConnectionState = false;
bool receivedFirstPacket = false;

// ==================================================
// MQTT PUBLISHING CONTROL
// ==================================================

bool publishingEnabled = true;

// ==================================================
// FUNCTION DECLARATIONS
// ==================================================

void OnDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *incomingData,
  int len
);

String getAlertMessage(struct_message data);
void printSensorReadings(struct_message data);
void publishSensorData(struct_message data);
void callback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void checkTransmitterConnection();
void processPacketSizeAlert();

// ==================================================
// ESP-NOW RECEIVE CALLBACK
// ESP32 ARDUINO CORE 3.x
// ==================================================

void OnDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *incomingData,
  int len
) {
  // Display received packet size
  Serial.printf(
    "\nESP-NOW packet received: %d bytes | "
    "Expected: %u bytes\n",
    len,
    (unsigned int)EXPECTED_PACKET_LENGTH
  );

  // Reject invalid packet length
  if (len < 0 ||
      (size_t)len != EXPECTED_PACKET_LENGTH) {

    portENTER_CRITICAL(&mux);

    invalidPacketSizeReceived = true;
    invalidReceivedLength = len;

    portEXIT_CRITICAL(&mux);

    Serial.println(
      "SECURITY ALERT: INVALID DATA SIZE!"
    );

    Serial.println(
      "Packet rejected. Sensor data unchanged."
    );

    return;
  }

  // Reject null data
  if (incomingData == nullptr) {

    Serial.println(
      "SECURITY ALERT: NULL PACKET DATA!"
    );

    Serial.println("Packet rejected.");

    return;
  }

  // Copy only after validating the packet length
  portENTER_CRITICAL(&mux);

  memcpy(
    &sensorData,
    incomingData,
    EXPECTED_PACKET_LENGTH
  );

  newDataReceived = true;

  portEXIT_CRITICAL(&mux);

  Serial.println(
    "Packet size valid. Packet accepted."
  );
}

// ==================================================
// PROCESS DATA-SIZE ALERT
// ==================================================

void processPacketSizeAlert() {

  bool alertAvailable = false;
  int receivedLength = 0;

  portENTER_CRITICAL(&mux);

  if (invalidPacketSizeReceived) {

    alertAvailable = true;

    receivedLength = invalidReceivedLength;

    invalidPacketSizeReceived = false;
  }

  portEXIT_CRITICAL(&mux);

  if (!alertAvailable) {
    return;
  }

  // Create detailed alert message
  String message =
    "DATA SIZE ALERT: Received " +
    String(receivedLength) +
    " bytes; expected " +
    String((unsigned int)EXPECTED_PACKET_LENGTH) +
    " bytes. Packet rejected.";

  Serial.println();
  Serial.println("========== DATA SIZE ALERT ==========");

  Serial.print("Received packet size: ");
  Serial.print(receivedLength);
  Serial.println(" bytes");

  Serial.print("Expected packet size: ");
  Serial.print(EXPECTED_PACKET_LENGTH);
  Serial.println(" bytes");

  Serial.println("Action: Packet rejected.");
  Serial.println("=====================================");

  // Publish alert if MQTT is connected
  if (client.connected()) {

    bool published = client.publish(
      alertTopic,
      message.c_str()
    );

    if (published) {
      Serial.println(
        "Data-size alert published to MQTT."
      );
    } else {
      Serial.println(
        "Failed to publish data-size alert."
      );
    }
  }
}

// ==================================================
// ALERT MESSAGE
// ==================================================

String getAlertMessage(struct_message data) {

  String message = "";

  if (data.temperatureAlert) {
    message += "Temperature alert; ";
  }

  if (data.humidityAlert) {
    message += "Humidity alert; ";
  }

  if (data.irOffAlert) {
    message += "IR off alert; ";
  }

  if (data.accelerationAlert) {
    message += "Acceleration alert; ";
  }

  if (message.length() == 0) {
    return "No active alerts";
  }

  message.remove(message.length() - 2);

  return message;
}

// ==================================================
// PRINT SENSOR READINGS
// ==================================================

void printSensorReadings(struct_message data) {

  Serial.println();
  Serial.println("========== SENSOR READINGS ==========");

  if (data.temperatureConnected) {

    Serial.printf(
      "Temperature: %.2f C\n",
      data.temperature
    );

  } else {

    Serial.println("Temperature: SENSOR ERROR");
  }

  if (data.humidityConnected) {

    Serial.printf(
      "Humidity: %.2f %%\n",
      data.humidity
    );

  } else {

    Serial.println("Humidity: SENSOR ERROR");
  }

  Serial.printf(
    "IR status: %s\n",
    data.irValue == LOW
      ? "OBJECT DETECTED"
      : "NO OBJECT / IR OFF"
  );

  if (data.accelerometerConnected) {

    Serial.printf(
      "Acceleration X: %.3f m/s^2\n",
      data.ax
    );

    Serial.printf(
      "Acceleration Y: %.3f m/s^2\n",
      data.ay
    );

    Serial.printf(
      "Acceleration Z: %.3f m/s^2\n",
      data.az
    );

    Serial.printf(
      "Acceleration magnitude: %.3f m/s^2\n",
      data.acceleration
    );

  } else {

    Serial.println("MPU6050: NOT CONNECTED");
  }

  Serial.println("------------ ALERT STATUS ------------");

  Serial.printf(
    "Temperature alert: %s\n",
    data.temperatureAlert ? "ACTIVE" : "NORMAL"
  );

  Serial.printf(
    "Humidity alert: %s\n",
    data.humidityAlert ? "ACTIVE" : "NORMAL"
  );

  Serial.printf(
    "IR off alert: %s\n",
    data.irOffAlert ? "ACTIVE" : "NORMAL"
  );

  Serial.printf(
    "Acceleration alert: %s\n",
    data.accelerationAlert ? "ACTIVE" : "NORMAL"
  );

  Serial.printf(
    "Overall alert: %s\n",
    data.alertActive ? "ACTIVE" : "NORMAL"
  );

  Serial.printf(
    "LED status: %s\n",
    data.alertActive ? "ON" : "OFF"
  );

  Serial.println("======================================");
}

// ==================================================
// PUBLISH SENSOR DATA AS JSON
// ==================================================

void publishSensorData(struct_message data) {

  if (!client.connected() || !publishingEnabled) {
    return;
  }

  JsonDocument doc;

  // Sensor readings
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["irValue"] = data.irValue;

  doc["ax"] = data.ax;
  doc["ay"] = data.ay;
  doc["az"] = data.az;

  doc["acceleration"] = data.acceleration;

  // Sensor connection status
  doc["temperatureConnected"] =
    data.temperatureConnected;

  doc["humidityConnected"] =
    data.humidityConnected;

  doc["irSensorConnected"] =
    data.irSensorConnected;

  doc["accelerometerConnected"] =
    data.accelerometerConnected;

  // Sensor alerts
  doc["temperatureAlert"] =
    data.temperatureAlert;

  doc["humidityAlert"] =
    data.humidityAlert;

  doc["irOffAlert"] =
    data.irOffAlert;

  doc["accelerationAlert"] =
    data.accelerationAlert;

  doc["alertActive"] =
    data.alertActive;

  doc["alertMessage"] =
    getAlertMessage(data);

  doc["transmitterConnected"] =
    transmitterConnected;

  // Serialize JSON
  char jsonBuffer[1024];

  size_t n = serializeJsonPretty(
    doc,
    jsonBuffer,
    sizeof(jsonBuffer)
  );

  if (n > 0 && n < sizeof(jsonBuffer)) {

    bool published = client.publish(
      sensorTopic,
      jsonBuffer
    );

    if (published) {
      Serial.println("Sensor data published to MQTT.");
    } else {
      Serial.println("MQTT sensor publishing failed.");
    }

  } else {

    Serial.println("ERROR: JSON data size invalid.");
  }

  // Publish sensor alert
  if (data.alertActive) {

    String message = getAlertMessage(data);

    client.publish(
      alertTopic,
      message.c_str()
    );
  }
}

// ==================================================
// MQTT COMMAND CALLBACK
// ==================================================

void callback(
  char* topic,
  byte* payload,
  unsigned int length
) {

  String command = "";

  for (unsigned int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

  command.trim();
  command.toLowerCase();

  Serial.print("MQTT command received: ");
  Serial.println(command);

  if (command == "hello") {

    client.publish(responseTopic, "hii");
  }

  else if (command == "start" ||
           command == "resume") {

    publishingEnabled = true;

    client.publish(
      responseTopic,
      "Sensor data publishing resumed"
    );

    Serial.println(
      "Sensor data publishing RESUMED."
    );
  }

  else if (command == "pause" ||
           command == "stop") {

    publishingEnabled = false;

    client.publish(
      responseTopic,
      command == "pause"
        ? "Sensor data publishing paused"
        : "Sensor data publishing stopped"
    );

    Serial.println(
      "Sensor data publishing PAUSED."
    );
  }

  else {

    client.publish(
      responseTopic,
      "Unknown command. Use hello, start, stop, pause, or resume."
    );
  }
}

// ==================================================
// MQTT RECONNECTION
// ==================================================

void reconnectMQTT() {

  while (!client.connected()) {

    if (WiFi.status() != WL_CONNECTED) {
      return;
    }

    String clientId =
      "ESP32_Receiver_" +
      String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.println(
      "Connecting to EMQX using MQTT over TLS..."
    );

    if (client.connect(clientId.c_str())) {

      Serial.println(
        "MQTT TLS connection established."
      );

      if (client.subscribe(commandTopic)) {

        Serial.println(
          "Subscribed to MQTT command topic."
        );
      }

      client.publish(
        responseTopic,
        "ESP32 receiver connected to MQTT over TLS"
      );

    } else {

      Serial.print("MQTT connection failed. State: ");
      Serial.println(client.state());

      Serial.println("Retrying in 2 seconds...");

      delay(2000);
    }
  }
}

// ==================================================
// TRANSMITTER CONNECTION STATUS
// ==================================================

void checkTransmitterConnection() {

  unsigned long now = millis();

  bool newConnectionState = false;

  portENTER_CRITICAL(&mux);

  if (receivedFirstPacket &&
      now - lastPacketTime < TRANSMITTER_TIMEOUT) {

    newConnectionState = true;
  }

  portEXIT_CRITICAL(&mux);

  transmitterConnected = newConnectionState;

  if (transmitterConnected != previousConnectionState) {

    previousConnectionState = transmitterConnected;

    if (client.connected()) {

      if (transmitterConnected) {

        client.publish(
          alertTopic,
          "TRANSMITTER CONNECTED"
        );

        Serial.println("Transmitter connected.");

      } else {

        client.publish(
          alertTopic,
          "TRANSMITTER DISCONNECTED"
        );

        Serial.println("Transmitter disconnected.");
      }
    }
  }
}

// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32 IoT RECEIVER STARTING");
  Serial.println("======================================");

  Serial.printf(
    "Expected ESP-NOW packet size: %u bytes\n",
    (unsigned int)EXPECTED_PACKET_LENGTH
  );

  // Connect Wi-Fi
  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // NEW: credentials from secrets.h

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected.");

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("Wi-Fi channel: ");
  Serial.println(WiFi.channel());

  // ------------------------------------------------
  // TLS CONFIGURATION
  // ------------------------------------------------
  // Temporary testing configuration.
  // Encryption is enabled, but server certificate
  // verification is disabled.
  //
  // Replace with espClient.setCACert(root_ca)
  // after installing the official EMQX CA certificate.
  // ------------------------------------------------

  espClient.setInsecure();

  // Configure MQTT
  client.setServer(mqtt_server, mqtt_port);

  client.setCallback(callback);

  client.setBufferSize(1536);

  client.setKeepAlive(60);

  client.setSocketTimeout(15);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {

    Serial.println(
      "ESP-NOW initialization failed!"
    );

    while (true) {
      delay(1000);
    }
  }

  if (esp_now_register_recv_cb(OnDataRecv) != ESP_OK) {

    Serial.println(
      "ESP-NOW callback registration failed!"
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println(
    "ESP-NOW initialized successfully."
  );

  lastPacketTime = 0;

  receivedFirstPacket = false;

  transmitterConnected = false;

  previousConnectionState = false;

  // Connect to MQTT
  reconnectMQTT();

  Serial.println("Receiver setup complete.");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {

  // Maintain Wi-Fi connection
  if (WiFi.status() != WL_CONNECTED) {

    WiFi.reconnect();

    delay(500);

    return;
  }

  // Maintain MQTT connection
  if (!client.connected()) {
    reconnectMQTT();
  }

  // Process MQTT commands
  client.loop();

  // Process invalid packet-size alerts even when
  // normal sensor publishing is paused.
  processPacketSizeAlert();

  // Copy the latest received ESP-NOW data
  struct_message receivedData;

  bool dataAvailable = false;

  portENTER_CRITICAL(&mux);

  if (newDataReceived) {

    memcpy(
      &receivedData,
      &sensorData,
      sizeof(receivedData)
    );

    newDataReceived = false;

    lastPacketTime = millis();

    receivedFirstPacket = true;

    dataAvailable = true;
  }

  portEXIT_CRITICAL(&mux);

  // Monitor transmitter connection
  checkTransmitterConnection();

  // Print and publish only when enabled
  if (dataAvailable && publishingEnabled) {

    printSensorReadings(receivedData);

    publishSensorData(receivedData);
  }

  delay(5);
}