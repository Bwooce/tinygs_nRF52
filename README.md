# TinyGS nRF52 — LoRa Satellite Ground Station on Thread

> # ⚠️ UNSUPPORTED UNOFFICIAL REIMPLEMENTATION
>
> **This is NOT the official TinyGS firmware.** It is an independent, experimental
> reimplementation for the nRF52840 platform. It has no connection to, and no
> endorsement from, the TinyGS project, its maintainers, or [tinygs.com](https://tinygs.com/).
>
> **No support is offered or implied.** Do not open issues or request help in the
> official TinyGS channels (Telegram, GitHub, Discourse, forums) about this
> firmware. If something doesn't work here, that is between you and this repo —
> not the TinyGS maintainers.
>
> **Do not report dashboard or server misbehaviour** observed while running this
> firmware to the TinyGS operators as if it were an official station. The station
> ID seen by the server is indistinguishable from an official node, but the
> behaviour is not.
>
> **Highly experimental / proof-of-concept.** Expect breakage, missing
> features, and protocol regressions. Not for production use.

## What is this?

TinyGS is an open network of ground stations that receive telemetry from LoRa satellites. The official firmware runs on ESP32 with WiFi. This project replaces the ESP32/WiFi stack with:

- **nRF52840** (Heltec Mesh Node T114) — ultra-low-power ARM Cortex-M4
- **OpenThread** (802.15.4 mesh) — replaces WiFi for network connectivity
- **Zephyr RTOS** (nRF Connect SDK) — replaces Arduino/ESP-IDF
- **NAT64** — routes MQTT traffic from the Thread mesh to the IPv4 TinyGS broker

The goal is a battery-friendly mesh-networked ground station as an alternative to the WiFi-based official firmware.

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
                                    NAT64 (mesh-synthesised, via Thread BorderRouting)
                                           │
                                    mqtt.tinygs.com:8883 (TLS)
```

- MQTT-TLS directly to the TinyGS broker — no proxies or gateways
- Protocol-compatible with ESP32 TinyGS nodes (same MQTT topics and JSON payloads)
- RadioLib for SX1262 control via a custom Zephyr HAL

## Current Status

### Working
- Thread mesh join via Joiner commissioning
- DNS + SNTP over OpenThread through NAT64 (synthesised from Thread netdata; no hardcoded IPv4 addresses)
- MQTT-TLS connection to mqtt.tinygs.com (ECDHE-RSA-AES256-GCM-SHA384)
- Welcome, ping, status, and RX packet publishing (ESP32-compatible JSON)
- LoRa packet reception with interrupt-driven DIO1, including Doppler correction from server-supplied TLE
- Server command handling (begine, batch_conf, set_pos_prm, status, set_name, get_adv_prm)
- USB composite device (CDC ACM console + MSC config drive)
- Last Will Testament (LWT) for disconnect detection
- 90s MQTT keepalive (reduced from 300s to detect half-open TCP sockets faster over NAT64)

### Not Yet Implemented
- OTA firmware updates (requires MCUboot migration)
- INA219 shunt for direct current measurement (power figures are currently inferred from Vbat drift vs LiPo curve, ±20%)
- Web UI for configuration (currently manual edit of `config.json` on the USB MSC drive)

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

Runtime config lives in `config.json` on the USB MSC drive exposed by the device
when running the application firmware (VID 2fe3:0001, appears as the
"TinyGS Configurator" drive). Mount the drive, edit `config.json`, unmount.

See [docs/CONFIG.md](docs/CONFIG.md) for the full list of configurable fields,
types, defaults, and how the file's lifecycle works.

Compiled-in fallback defaults live in `src/mqtt_credentials.h` (gitignored):

```c
#define MQTT_USERNAME "your_tinygs_username"
#define MQTT_PASSWORD "your_tinygs_password"
```

Runtime `config.json` overrides these on every boot.

### Thread Border Router setup

This station needs a BR that runs **DNS64 + NAT64 + `ot-ctl` commissioner**.
Apple TV / HomePod and Google Nest Thread BRs only forward Matter/HomeKit
traffic — they don't do NAT64 and can't commission, so they won't work as
the egress for this station (though they can coexist on the same mesh).

See [docs/HA_OTBR_SETUP.md](docs/HA_OTBR_SETUP.md) for the full guide:
HA OpenThread Border Router add-on install, `br routeprf high` to beat
Apple/Google BRs to the HIGH route preference, the `ip6tables` MASQUERADE
rule required for Thread ULA egress, and the HA automation to make both
survive a reboot.

Once the BR is set up, commissioning the device itself is two commands:

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
| Power (always-on) | ~100 mA active | ~15–20 mA active (Vbat-inferred, ±20%) |
| IP | IPv4 native | IPv6 via mesh-synthesised NAT64 |
| Config | Web UI | USB Mass Storage drive (`config.json`) |
| JSON parsing | ArduinoJson | Zephyr `json.h` descriptors |
| JSON output | ArduinoJson | `snprintf` |
| Radio wrapper | `RadioHal<T>` over RadioLib (multi-chip) | RadioLib directly (SX1262 only) |

## MQTT Protocol

The MQTT protocol was reverse-engineered from the ESP32 TinyGS source code. See [docs/TINYGS_MQTT_PROTOCOL.md](docs/TINYGS_MQTT_PROTOCOL.md) for the full specification including topic structure, JSON payload formats, and field types.

## Project Structure

```
src/
  main.cpp              — State machine: Thread join, DNS, MQTT-TLS, LoRa RX
  tinygs_protocol.cpp/h — MQTT payload builders and command handlers
  tinygs_json.cpp/h     — Begine/set_pos_prm JSON parsing (Zephyr json.h)
  tinygs_config.cpp/h   — Runtime config loading (NVS + config.json)
  tinygs_ca_cert.h      — TinyGS server TLS CA cert
  mqtt_credentials.h    — Compiled-in fallback credentials (gitignored)
  mbedtls-user-config.h — mbedTLS RSA/TLS overrides
  AioP13.*              — Satellite orbit propagator for Doppler
  bitcode.*             — AX.25 / PN9 / NRZS codec for FSK post-processing
  hal/
    ZephyrHal.cpp/h     — Platform HAL for RadioLib (GPIO, SPI, IRQ, delay)
lib/
  RadioLib/             — LoRa/FSK radio library (submodule)
docs/
  TINYGS_MQTT_PROTOCOL.md — Reverse-engineered MQTT protocol spec
  HA_OTBR_SETUP.md        — Home Assistant OpenThread BR setup guide
  CONFIG.md               — config.json field reference
tests/
  json_parser/          — Zephyr unit tests for JSON begine parser
scripts/
  serial_log.py         — Serial port logger with timestamps
```

## License

MIT License. See [LICENSE](LICENSE).

This project is an independent reimplementation. No code was copied from the ESP32 TinyGS firmware (which is GPL-3.0). The MQTT protocol compatibility was achieved through clean-room reverse engineering of the protocol specification.

[RadioLib](https://github.com/jgromes/RadioLib) is used under the MIT license.
