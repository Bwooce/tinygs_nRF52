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
| Client ID | 12-char hex from device MAC: `%04X%08X` |
| Username | Per-station credential from TinyGS dashboard |
| Password | Per-station credential from TinyGS dashboard |
| Max packet size | 1000 bytes |
| Keep-alive | Default (60s from Zephyr MQTT lib) |
| TLS | Required. TinyGS custom CA + ISRG Root X1 (see `certs.h`) |

## 2. MQTT Topics

### 2.1 Subscriptions

| Topic | Purpose |
|-------|---------|
| `tinygs/global/#` | Global broadcasts (batch config, OTA, reset, status requests) |
| `tinygs/{user}/{station}/cmnd/#` | Station-specific commands |

Where `{user}` and `{station}` are from the station's configuration.

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

```json
{
  "station_location": [lat, lon],
  "version": "2411220",
  "git_version": "abc1234",
  "chip": "ESP32-D0WDQ6",
  "board": 67,
  "mac": "AABBCCDDEEFF",
  "ip": "192.168.1.100",
  "tx": 0,
  "lp": 0,
  "modem_conf": { ... },
  "boardTemplate": "...",
  "sat": "...",
  "radioChip": 6,
  "Mem": 123456,
  "Size": 1310720,
  "MD5": "...",
  "seconds": 0,
  "Vbat": 3.7,
  "slot": 0,
  "pSize": 1310720,
  "idfv": "v4.4"
}
```

### 3.2 Ping (`tele/ping`)

```json
{
  "Vbat": 3.7,
  "Mem": 123456,
  "MinMem": 100000,
  "MaxBlk": 65536,
  "RSSI": -65,
  "radio": 1,
  "InstRSSI": -100
}
```

### 3.3 RX Packet (`tele/rx`)

**LoRa mode:**
```json
{
  "station_location": [lat, lon],
  "mode": 1,
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
  "crc_error": 0,
  "data": "base64_encoded_packet",
  "NORAD": 46494,
  "noisy": 0,
  "iIQ": 0
}
```

**FSK mode** (additional fields):
```json
{
  "mode": 2,
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
