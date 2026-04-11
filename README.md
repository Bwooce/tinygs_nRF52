# TinyGS nRF52 — LoRa Satellite Ground Station on Thread

An **unofficial, unendorsed** port of [TinyGS](https://tinygs.com/) to the nRF52840 platform using Zephyr RTOS and OpenThread mesh networking.

> **Status: Highly experimental.** This is a proof-of-concept. Expect rough edges, missing features, and breaking changes. Not affiliated with or endorsed by the TinyGS project.

## What is this?

TinyGS is an open network of ground stations that receive telemetry from LoRa satellites. The official firmware runs on ESP32 with WiFi. This project replaces the ESP32/WiFi stack with:

- **nRF52840** (Heltec Mesh Node T114) — ultra-low-power ARM Cortex-M4
- **OpenThread** (802.15.4 mesh) — replaces WiFi for network connectivity
- **Zephyr RTOS** (nRF Connect SDK) — replaces Arduino/ESP-IDF
- **NAT64** — routes MQTT traffic from the Thread mesh to the IPv4 TinyGS broker

The goal is a solar-powered ground station with deep sleep capability, using Thread's mesh networking instead of WiFi.

## Hardware

| Component | Details |
|-----------|---------|
| Board | Heltec Mesh Node T114 |
| MCU | nRF52840 (256KB RAM, 1MB flash) |
| Radio | SX1262 LoRa (via SPI) |
| Network | 802.15.4 Thread (OpenThread MTD) |
| Bootloader | Adafruit UF2 (USB flashing, no SWD required) |

## Architecture

```
Satellite ──(LoRa)──> SX1262 ──(SPI)──> nRF52840
                                           │
                                    OpenThread MTD
                                           │
                                    Thread Border Router
                                           │
                                    NAT64 (nat64.net)
                                           │
                                    mqtt.tinygs.com:8883 (TLS)
```

- MQTT-TLS directly to the TinyGS broker — no proxies or gateways
- Protocol-compatible with ESP32 TinyGS nodes (same MQTT topics and JSON payloads)
- RadioLib for SX1262 control via a custom Zephyr HAL

## Current Status

### Working
- Thread mesh join via Joiner commissioning
- DNS resolution via OpenThread DNS client + nat64.net DNS64
- MQTT-TLS connection to mqtt.tinygs.com (ECDHE-RSA-AES256-GCM-SHA384)
- Welcome, ping, and RX packet publishing (ESP32-compatible JSON)
- LoRa packet reception with interrupt-driven DIO1
- Server command handling (begine, batch_conf, set_pos_prm, status)
- USB composite device (CDC ACM console + MSC config drive)
- 600s MQTT keepalive confirmed stable over NAT64
- Last Will Testament (LWT) for disconnect detection

### Not Yet Implemented
- Config persistence (NVS/config.json)
- SED sleep mode (Phase 3)
- OTA firmware updates (requires MCUboot migration)
- Full satellite tracking (waiting for server to assign satellites)

## Building

Requires nRF Connect SDK (NCS) v3.5.99-ncs1.

```bash
# Set up the environment (first time only)
./setup_zephyr.sh

# Build
./build.sh

# Flash via UF2 bootloader
./flash.sh

# Monitor serial output
python3 scripts/serial_log.py /dev/ttyACM0 115200 serial.log
```

## Configuration

1. Create `src/mqtt_credentials.h` (gitignored):
   ```c
   #define MQTT_USERNAME "your_tinygs_username"
   #define MQTT_PASSWORD "your_tinygs_password"
   ```

2. Set your station name in `src/main.cpp`:
   ```c
   #define MQTT_CLIENT_ID "your_station_name"
   ```

3. Commission the device onto your Thread network:
   ```bash
   # On your OpenThread Border Router:
   ot-ctl commissioner start
   ot-ctl commissioner joiner add '*' TNYGS2026NRF
   ```

## Key Differences from ESP32 TinyGS

| Feature | ESP32 TinyGS | This Port |
|---------|-------------|-----------|
| Network | WiFi | OpenThread (802.15.4 mesh) |
| MCU | ESP32/ESP32-S3 | nRF52840 |
| RTOS | Arduino/ESP-IDF | Zephyr RTOS |
| Power | ~100mA active | Target: 11uA deep sleep |
| IP | IPv4 native | IPv6 via NAT64 |
| Config | Web UI | USB Mass Storage drive |
| JSON | ArduinoJson | snprintf (no library dependency) |

## MQTT Protocol

The MQTT protocol was reverse-engineered from the ESP32 TinyGS source code. See [docs/TINYGS_MQTT_PROTOCOL.md](docs/TINYGS_MQTT_PROTOCOL.md) for the full specification including topic structure, JSON payload formats, and field types.

## Project Structure

```
src/
  main.cpp              — State machine, MQTT, Thread, LoRa
  tinygs_protocol.cpp/h — MQTT payload builders and command handlers
  tinygs_ca_cert.h      — TinyGS server TLS certificate
  mqtt_credentials.h    — MQTT credentials (gitignored)
  mbedtls-user-config.h — mbedTLS RSA/TLS overrides
  hal/
    ZephyrHal.cpp/h     — RadioLib HAL for Zephyr (GPIO, SPI, interrupts)
lib/
  RadioLib/             — LoRa radio library (submodule)
docs/
  TINYGS_MQTT_PROTOCOL.md — Reverse-engineered MQTT protocol spec
scripts/
  serial_log.py         — Serial port logger with timestamps
```

## License

MIT License. See [LICENSE](LICENSE).

This project is an independent reimplementation. No code was copied from the ESP32 TinyGS firmware (which is GPL-3.0). The MQTT protocol compatibility was achieved through clean-room reverse engineering of the protocol specification.

[RadioLib](https://github.com/jgromes/RadioLib) is used under the MIT license.
