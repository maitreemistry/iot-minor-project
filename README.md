# Smart Helmet for Mine Workers — IoT Security Project

A two-ESP32 smart-helmet prototype that collects environmental and movement readings, generates local alerts, and publishes monitoring data through MQTT. This README describes what the current transmitter and receiver firmware do and outlines the next step: completing certificate-verified MQTT over TLS.

## 1. Project overview

The system has three main parts:

1. **Helmet transmitter (ESP32):** reads sensors, calculates alert flags, and sends a fixed-size data packet over ESP-NOW.
2. **Gateway receiver (ESP32):** receives and validates ESP-NOW packets, detects packet loss and transmitter timeouts, drives a buzzer, and publishes sensor/alert reports through MQTT over Wi-Fi.
3. **MQTTX dashboard/client:** subscribes to MQTT topics to view the data and alerts.

### Data flow

```text
DHT22 + IR sensor + MPU6050
             |
             v
     ESP32 Transmitter
             |
      ESP-NOW (local link)
             |
             v
      ESP32 Receiver/Gateway ----> Buzzer
             |
       Wi-Fi + MQTT/TLS
             |
             v
       MQTT broker
             |
             v
       MQTTX dashboard
```

> **Current security status:** the receiver is configured for the TLS port (`8883`) and uses `WiFiClientSecure`, but it currently calls `setInsecure()`. That encrypts the connection without validating the broker certificate. The TLS implementation is therefore **not yet certificate-verified**. The next step is to configure a trusted CA certificate and test certificate validation.

## 2. Hardware and software

### Hardware

- 2 × ESP32 development boards
- DHT22 temperature/humidity sensor
- IR digital sensor
- MPU6050 accelerometer/gyroscope module
- Buzzer connected to receiver GPIO 25
- LED connected to transmitter GPIO 26
- Breadboard, jumper wires, and suitable power supplies

### Software and libraries

- Arduino IDE and ESP32 board support package
- `WiFi.h`, `esp_now.h`, `esp_wifi.h`
- `WiFiClientSecure.h` (receiver TLS transport)
- `PubSubClient`
- `ArduinoJson`
- `DHT`
- `Wire`
- `Adafruit_MPU6050`
- `Adafruit_Sensor`

Install any missing third-party libraries using Arduino IDE Library Manager.

## 3. What the existing transmitter code does

File: `Transmitter_Code_Final.ino`

- Reads temperature and humidity from the DHT22 connected to GPIO 22.
- Reads the digital IR sensor on GPIO 5.
- Reads acceleration values from the MPU6050 over I²C (SDA GPIO 21, SCL GPIO 18).
- Calculates temperature, humidity, IR, and acceleration alert flags.
- Turns the transmitter LED on when the combined alert flag is active.
- Prints sensor readings and ESP-NOW delivery statistics to Serial Monitor at `115200` baud.
- Sends the sensor structure to the receiver using ESP-NOW.
- Tracks accepted sends and delivery callback successes/failures.
- Includes `secrets.h` for Wi-Fi configuration. Wi-Fi is initialized on the transmitter, and the board attempts to connect with a timeout.

### Transmitter thresholds currently in the source

| Condition | Current rule |
|---|---|
| High temperature | Above 30 °C |
| Humidity out of range | Below 20% or above 80% |
| Acceleration | Absolute value of any axis above 20 m/s² |
| IR status | `HIGH` is treated as an IR alert |

These are prototype thresholds. Calibrate them against the actual sensors and intended environment before relying on them for safety decisions.

## 4. What the existing receiver code does

File: `Receiver_Code_Final.ino`

### ESP-NOW receive and validation

- Connects the receiver to Wi-Fi using credentials from `secrets.h`.
- Initializes ESP-NOW and registers the receive callback.
- Expects the same `struct_message` layout as the transmitter; the source includes a compile-time assertion that the structure is 40 bytes.
- Rejects a null or incorrectly sized incoming packet before copying it into the sensor structure.
- Counts received and invalid packets and estimates missed packets from the time gap between valid packets.
- Tracks the most recent packet time and raises an alert if the transmitter stops sending for the configured timeout.

### Alerts and buzzer

- Builds a readable alert message from sensor status and alert flags.
- Activates the receiver buzzer on GPIO 25 when an alert is active, the transmitter times out, packet loss is detected, or an invalid packet size is detected.
- Publishes sensor reports and alert reports to MQTT.
- Supports MQTT commands on the command topic to pause/resume sensor publishing and sends a response to the response topic.

### MQTT configuration currently in the source

| Setting | Current value |
|---|---|
| Broker hostname | `broker.emqx.io` |
| Port | `8883` |
| Sensor topic | `emqx/esp32/iot_project` |
| Command topic | `emqx/esp32/iot_project/command` |
| Response topic | `emqx/esp32/iot_project/response` |
| Alert topic | `emqx/esp32/iot_project/alert` |
| MQTT client library | PubSubClient |
| TLS client | `WiFiClientSecure` |

The code uses a generated receiver client ID and reconnects to MQTT when disconnected. The public broker and topic names are suitable for development demonstrations only; public topics can be shared with unrelated users.

## 5. Existing security measures

The current code already includes these useful protections and resilience features:

- Wi-Fi credentials are loaded from `secrets.h` rather than embedded directly in the main sketches.
- The ESP-NOW receiver checks packet length before copying packet data.
- Packet reception, invalid packet counts, estimated packet loss, and transmitter timeout are tracked.
- Wi-Fi/MQTT reconnection is attempted.
- The receiver treats missing transmitter data and invalid packet sizes as alert conditions.

These measures do **not** secure every communication path. The ESP-NOW link is currently configured with `peerInfo.encrypt = false`, so ESP-NOW payload encryption and sender authentication remain future work. Storing credentials in `secrets.h` also does not encrypt them in device flash.

## 6. Next step: finish MQTT TLS correctly

### Current limitation

In `Receiver_Code_Final.ino`, the setup currently contains:

```cpp
espClient.setInsecure();
client.setServer(mqtt_server, mqtt_port);
```

`setInsecure()` disables certificate verification. Do not treat this as production-ready TLS: a connection may be encrypted while the ESP32 still cannot verify that it is talking to the genuine broker.

### Implementation plan

1. **Obtain the correct CA certificate.** Get the CA certificate chain from the broker/service's official documentation or administrator. Confirm that it is valid for the exact hostname used by the ESP32 (`broker.emqx.io`). Do not blindly trust a certificate copied from an unofficial source.
2. **Store the CA certificate in the firmware.** Add a header such as `broker_ca.h` containing the PEM certificate as a C string. Keep the certificate text intact, including the `BEGIN CERTIFICATE` and `END CERTIFICATE` lines.
3. **Include the CA header in the receiver.** Replace the insecure setting with certificate validation, for example:

   ```cpp
   #include <WiFiClientSecure.h>
   #include "broker_ca.h"

   // After Wi-Fi has connected:
   espClient.setCACert(BROKER_ROOT_CA);
   client.setServer(mqtt_server, mqtt_port);
   ```

   Define `BROKER_ROOT_CA` in `broker_ca.h` as the PEM string. The certificate must chain to the certificate actually presented by the broker. Keep the broker hostname unchanged so TLS hostname validation can work.
4. **Check the ESP32 clock if certificate validation fails.** Certificate validity depends on the current date/time. Synchronize time using NTP after Wi-Fi connects and before the TLS handshake; wait until the clock is set.
5. **Use unique credentials and access controls.** Prefer a private broker or managed broker configuration with per-device credentials and topic-level permissions. Do not assume public broker credentials or public topics provide access control.
6. **Test normal and failure cases.** Verify that the receiver connects with the valid CA; sensor data and alerts continue to arrive in MQTTX; reconnect works after Wi-Fi/broker interruption; and TLS fails when the CA is missing, incorrect, expired, or otherwise untrusted. Never resolve certificate errors by reverting to `setInsecure()`.
7. **Protect secrets.** Keep `secrets.h` and any private keys out of Git. A public CA certificate is not a private key, but Wi-Fi passwords, broker passwords, and client private keys must remain secret.

### TLS verification checklist

- [ ] Receiver connects to Wi-Fi.
- [ ] Correct CA certificate is installed in firmware.
- [ ] ESP32 clock is synchronized before certificate verification.
- [ ] `setInsecure()` is removed.
- [ ] Receiver uses `setCACert(...)`.
- [ ] MQTT connection succeeds on port `8883`.
- [ ] MQTTX receives sensor and alert messages.
- [ ] Invalid/untrusted certificate causes the TLS connection to fail.
- [ ] Wi-Fi/MQTT reconnection is tested.
- [ ] No secrets are committed to the repository.

## 7. Suggested implementation and testing sequence

1. Back up the currently working transmitter and receiver sketches.
2. Confirm both sketches use exactly the same `struct_message` field order and data types.
3. Test the transmitter's sensors and ESP-NOW delivery first.
4. Test receiver packet validation, buzzer behavior, and timeout behavior.
5. Configure and test verified MQTT TLS separately.
6. Test the complete end-to-end flow: sensors → ESP-NOW → receiver → verified MQTT TLS → MQTTX.
7. Record the successful connection log and the negative test (untrusted CA rejected) as project evidence.

## 8. Further security work

After certificate-verified MQTT TLS is complete, consider:

- Enabling ESP-NOW encryption with correctly managed PMK/LMK keys.
- Restricting accepted ESP-NOW peers and validating sender identity.
- Adding a message counter, timestamp or nonce to reduce replay risk.
- Moving from a public broker to a private broker with unique credentials and per-device topic access control.
- Improving watchdog/recovery behavior and local buffering during network outages.
- Evaluating secure boot, flash encryption, and physical protection of the ESP32.
- Calibrating sensor thresholds and testing fail-safe behavior under realistic conditions.

## 9. Troubleshooting

| Symptom | Checks |
|---|---|
| ESP-NOW packets are rejected | Confirm both sketches use the same 40-byte structure, field order, data types, and compatible ESP32 board package/callback API. |
| No ESP-NOW data arrives | Verify receiver MAC address, power, radio channel compatibility, and Serial Monitor output on both boards. |
| MQTT connection fails | Check Wi-Fi, hostname, port, broker availability, client configuration, and MQTT client state output. |
| TLS certificate verification fails | Confirm CA chain, hostname, certificate validity dates, and ESP32 system time. Do not disable validation as a workaround. |
| MQTTX shows no messages | Subscribe to the exact topic, check broker/port/TLS settings in MQTTX, and confirm the receiver reports successful publishing. |
| Buzzer stays active | Check sensor flags, transmitter timeout, packet-loss state, and packet-size validation messages. |

## 10. Important safety note

This is an educational prototype, not a certified mine-safety device. Sensor thresholds, gas-sensor calibration, radio reliability, power failure behavior, and alert response must be validated before any real-world use. Do not rely on this prototype as the sole means of protecting workers.

---

**Project:** Smart Helmet for Mine Workers  
**Communication:** ESP-NOW between ESP32 nodes; MQTT over TLS planned for gateway-to-broker communication  
**Serial Monitor:** `115200` baud  
