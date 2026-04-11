# TinyGS MQTT Protocol Specification

> **Note:** This specification was reverse-engineered from the ESP32 TinyGS firmware source code
> at [github.com/G4lile0/tinyGS](https://github.com/G4lile0/tinyGS) (commit at time of analysis).
> It is not an official specification and may be incomplete or outdated. Use the ESP32 source as
> the authoritative reference.

## 1. Connection Parameters

| Parameter | Value |
|-----------|-------|
| Broker | mqtt.tinygs.com |
| Port | 8883 (TLS) |
| Client ID | 12-char hex from device MAC: `%04X%08X` (e.g., `16EDE3EB6081`) |
| Username | Per-station credential from TinyGS dashboard |
| Password | Per-station credential from TinyGS dashboard |
| Max packet size | 1000 bytes |
| Keep-alive | Configurable via CONFIG_MQTT_KEEPALIVE. Tested: 300s and 600s both work over NAT64. 600s (10 min) is the practical maximum — longer is unnecessary since the server sends radio config updates frequently. |
| TLS | Required. TinyGS custom CA + ISRG Root X1 (see `certs.h`) |

### 1.1 Client ID vs Station Name

**IMPORTANT:** The MQTT client ID and the station name in topic paths are different:

| Concept | Source | Used for | Example |
|---------|--------|----------|---------|
| **Client ID** | Device MAC (`NRF_FICR->DEVICEID`) | MQTT `connect()` call | `16EDE3EB6081` |
| **Station name** | TinyGS dashboard config | Topic paths, `{station}` in all topics | `tinygs_nrf52_poc` |
| **mac field** | Device MAC | `mac` field in welcome JSON payload | `16EDE3EB6081` |

The station name determines which station entry appears on the TinyGS portal. Using
the MAC as the station name creates a new, separate station entry. The client ID is
only used by the MQTT broker for session management and does not appear in topics.

## 2. MQTT Topics

### 2.1 Subscriptions

| Topic | Purpose |
|-------|---------|
| `tinygs/global/#` | Global broadcasts (batch config, OTA, reset, status requests) |
| `tinygs/{user}/{station}/cmnd/#` | Station-specific commands |

Where `{user}` is the MQTT username and `{station}` is the dashboard-configured station name (NOT the MAC-derived client ID).

### 2.3 Enabling Auto-Tune (Satellite Assignment)

The server only assigns satellites to stations with auto-tune enabled. This is a
server-side setting toggled via the TinyGS web dashboard, NOT an MQTT field.

To enable: the device publishes `1` to `tinygs/{user}/{station}/tele/get_weblogin`.
The server responds on `tinygs/{user}/{station}/cmnd/weblogin` with a one-time login
URL (e.g., `https://tinygs.com?loginToken=...&userId=...`). Open this URL in a browser
to access the station's Operator settings, where "Auto Tune" can be toggled ON.

Once enabled, the server sends `begine` commands to the station-specific cmnd topic
with satellite frequency, modulation parameters, and satellite name.

### 2.2 Published Topics (Telemetry)

| Topic | When | Purpose |
|-------|------|---------|
| `tinygs/{user}/{station}/tele/welcome` | On connect/restart | Device info, version, config |
| `tinygs/{user}/{station}/tele/ping` | Every 60s | Heartbeat with battery, memory, RSSI |
| `tinygs/{user}/{station}/tele/rx` | On packet receive | LoRa/FSK received packet (base64) |
| `tinygs/{user}/{station}/tele/get_adv_prm` | On request | Advanced parameters response |
| `tinygs/{user}/{station}/tele/get_weblogin` | On request | Web login URL |

### 2.3 Command Topics Handled (23 total)

**Global commands** (`tinygs/global/cmnd/`):
- `batch_conf` — Batch radio configuration
- `update` — OTA firmware update
- `reset` — Hard reset device
- `status` — Request status report
- `log` — Server log message
- `weblogin` — Web login request

**Station commands** (`tinygs/{user}/{station}/cmnd/`):
- `freq` — Set frequency
- `beginp` — Persist modem config
- `begine` — Load modem config
- `begin_lora` — Quick LoRa configuration
- `begin_fsk` — Quick FSK configuration
- `sat` — Select satellite
- `tx` — Transmit data
- `filter` — Packet filter
- `sleep` — Deep sleep
- `siesta` — Light sleep
- `foff` — Frequency offset
- `set_adv_prm` — Set advanced parameters
- `get_adv_prm` — Get advanced parameters
- `set_pos_prm` — Set station coordinates
- `set_name` — Set station name
- `sat_pos_oled` — Display satellite position
- `frame/{num}` — Display frame data

## 3. JSON Payload Formats

### 3.1 Welcome (`tele/welcome`)

Fields and their exact types as serialized by ESP32 ArduinoJson:

| Field | JSON type | C++ source type | Example | Notes |
|-------|-----------|-----------------|---------|-------|
| station_location | array[float,float] | float | [-33.87, 151.21] | [lat, lon] |
| version | number | uint32_t | 2603242 | YYMMDDR format (NOT a string) |
| git_version | string | const char* | "abc1234" | Git commit hash |
| chip | string | const char* | "ESP32-D0WDQ6" | Chip model |
| board | number | uint8_t | 67 | Board index into boards[] array |
| mac | string | char[13] | "AABBCCDDEEFF" | Device MAC, 12 hex chars |
| radioChip | number | uint8_t | 6 | Enum: 0=SX1262, 1=SX1278, 2=SX1276, 5=SX1268, 6=SX1262, 10=LR1121 |
| Mem | number | uint32_t | 98304 | Free heap bytes |
| seconds | number | uint32_t | 3600 | Uptime in seconds |
| Vbat | number | int | 4200 | Battery voltage in **millivolts** (NOT volts) |
| tx | boolean | bool | false | Whether TX is allowed |
| sat | string | char* | "NORAD-50465" | Current satellite name |
| ip | string | String | "192.168.1.100" | WiFi IP (nRF52: "0.0.0.0") |
| idfv | string | const char* | "v5.1.2" | IDF/SDK version |
| modem_conf | string | string | "{}" | Modem startup config (JSON as string) |
| time | number | time_t | 1712849042 | Unix timestamp (optional) |
| lp | boolean | bool | false | Low power mode (optional) |
| boardTemplate | string | const char* | "lilygo-t-beam" | Board template name (optional) |
| Size | number | uint32_t | 1310720 | Sketch/firmware size (optional) |
| MD5 | string | String | "a1b2c3d4..." | Firmware MD5 hash (optional) |
| slot | string | const char* | "app0" | OTA partition label (optional) |
| pSize | number | uint32_t | 1966080 | Partition size (optional) |

```json
{
  "station_location": [-33.8688, 151.2093],
  "version": 2604100,
  "git_version": "tinygs_nRF52",
  "chip": "nRF52840",
  "board": 255,
  "mac": "AABB11223344",
  "radioChip": 6,
  "Mem": 81920,
  "seconds": 3600,
  "Vbat": 3700,
  "tx": false,
  "sat": "",
  "ip": "0.0.0.0",
  "idfv": "NCS/Zephyr",
  "modem_conf": "{}"
}
```

### 3.2 Ping (`tele/ping`)

| Field | JSON type | C++ source type | Example | Notes |
|-------|-----------|-----------------|---------|-------|
| Vbat | number | int | 4200 | Battery voltage in **millivolts** |
| Mem | number | uint32_t | 98304 | Free heap bytes |
| MinMem | number | uint32_t | 65536 | Minimum free heap (historical) |
| MaxBlk | number | uint32_t | 131072 | Max allocatable block |
| RSSI | number | int32_t | -45 | WiFi/Thread parent RSSI (dBm) |
| radio | number | int16_t | 0 | RadioLib error code (0=OK) |
| InstRSSI | number | float | -98.5 | Instantaneous radio RSSI |

```json
{
  "Vbat": 3700,
  "Mem": 81920,
  "MinMem": 65536,
  "MaxBlk": 81920,
  "RSSI": -45,
  "radio": 0,
  "InstRSSI": -120.0
}
```

### 3.3 RX Packet (`tele/rx`)

| Field | JSON type | C++ source type | Example | Notes |
|-------|-----------|-----------------|---------|-------|
| station_location | array[float,float] | float | [-33.87, 151.21] | [lat, lon] |
| mode | **string** | char[8] | "LoRa" | "LoRa", "FSK", or "GMSK" (NOT a number) |
| frequency | number | float | 436.703 | MHz |
| frequency_offset | number | float | 0 | Hz |
| satellite | string | char* | "NORBI" | Satellite name |
| sf | number | uint8_t | 10 | Spreading factor (LoRa only) |
| cr | number | uint8_t | 5 | Coding rate (LoRa only) |
| bw | number | float | 250.0 | Bandwidth kHz (LoRa only) |
| iIQ | **boolean** | bool | false | Inverted IQ (LoRa only) |
| bitrate | number | float | 9600.0 | Bitrate (FSK only) |
| freqdev | number | float | 5000.0 | Freq deviation (FSK only) |
| rxBw | number | float | 25000.0 | RX bandwidth (FSK only) |
| data_raw | string | char* | "AQID..." | Base64 raw bits (FSK only) |
| rssi | number | float | -120.5 | Packet RSSI |
| snr | number | float | -5.25 | Packet SNR |
| frequency_error | number | float | 1234.5 | Hz |
| unix_GS_time | number | time_t | 1700000000 | Unix timestamp at reception |
| usec_time | number | int64_t | 123456 | Microsecond timestamp |
| crc_error | **boolean** | bool | false | CRC check result |
| data | string | char* | "base64..." | Base64 encoded packet |
| NORAD | number | uint32_t | 46494 | NORAD catalog number |
| noisy | **boolean** | bool | false | Noisy packet flag |

**LoRa mode:**
```json
{
  "station_location": [-33.8688, 151.2093],
  "mode": "LoRa",
  "frequency": 436.703,
  "frequency_offset": 0,
  "satellite": "NORBI",
  "sf": 10,
  "cr": 5,
  "bw": 250.0,
  "rssi": -120.5,
  "snr": -5.25,
  "frequency_error": 1234.5,
  "unix_GS_time": 1700000000,
  "usec_time": 123456,
  "crc_error": false,
  "data": "base64_encoded_packet",
  "NORAD": 46494,
  "noisy": false,
  "iIQ": false
}
```

**FSK mode** (additional/different fields):
```json
{
  "mode": "FSK",
  "bitrate": 9600,
  "freqdev": 5000,
  "rxBw": 25000,
  "data_raw": "base64_raw_bits"
}
```

### 3.4 Status (`stat/status`)

Published in response to `cmnd/status`. Contains station location, version, board, tx status,
current modem configuration (mode, frequency, satellite, SF/CR/BW or bitrate/freqdev/rxBw),
and last packet metrics (rssi, snr, frequency_error, crc_error, timestamps).

## 4. Radio Configuration (ModemInfo)

The modem configuration structure contains:
- **Common:** satellite name, modem_mode (1=LoRa, 2=FSK), frequency, freqOffset, NORAD, TLE
- **LoRa:** sf (7-12), cr (5-8), bw, sw (sync word, default 18), iIQ (inverted IQ)
- **FSK:** bitrate, freqDev, rxBw, OOK, encoding, frame sync word, whitening seed, framing type
- **CRC:** crc enable, sw CRC, nbytes, init, poly, finalxor, refIn, refOut
- **Other:** power, preambleLength, gain, fldro, filter[], payload length

## 5. Packet Reception Flow

1. **ISR:** `Radio::setFlag()` sets a volatile boolean flag
2. **Main loop:** `radio.listen()` checks flag, calls `readData()`
3. **Read:** Get packet length, read data buffer, capture RSSI/SNR/timestamp
4. **Process:** Optional FSK frame decode, CRC check, apply packet filters
5. **Queue:** Base64-encode payload → `queueRx()` (max 10 packets, drops oldest)
6. **Publish:** `processRxQueue()` sends up to 2 packets per MQTT loop iteration

## 6. Timing

| Operation | Interval |
|-----------|----------|
| MQTT ping | 60s (PubSubClient default) |
| RX queue drain | 2 packets per loop |
| TLE refresh display | 4000ms |
| Reconnect base delay | 20s + random(10-20s) |
| Max reconnect attempts | 6 (then deep sleep 4h or restart) |

## 7. Certificates

The ESP32 firmware embeds two root CA certificates in `certs.h`:
1. **TinyGS custom CA** — self-signed CA for mqtt.tinygs.com
2. **ISRG Root X1** — Let's Encrypt root (for fallback/CDN)

The nRF52 port embeds the TinyGS Intermediary CA in `src/tinygs_ca_cert.h`.

## 8. ESP32 Source Reference

Source: [github.com/G4lile0/tinyGS](https://github.com/G4lile0/tinyGS)
Key files:
- `src/Mqtt/MQTT_Client.cpp` — Connection, subscriptions, command dispatch
- `src/Mqtt/MQTT_credentials.cpp` — Credential management
- `src/Radio/Radio.cpp` — RadioLib integration, packet RX/TX
- `src/Status.h` — Telemetry data structures
- `src/ConfigManager/ConfigManager.cpp` — Settings persistence
- `tinyGS.ino` — Main loop and state machine
