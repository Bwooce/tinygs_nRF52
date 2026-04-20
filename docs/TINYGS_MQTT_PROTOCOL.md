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
| Keep-alive | Configurable via CONFIG_MQTT_KEEPALIVE. Currently **90 s** (ping every 60 s). Tested: 300 s and 600 s both work over NAT64. 600 s is the practical maximum before broker/NAT64 conntrack flakiness. 90 s is our operational default for fast detection of Thread-flap-induced half-open sockets (see §"Why 90 s"). |
| MQTT version | 3.1.1 (NOT 5.0 — server does not support MQTT 5.0) |
| LWT (Last Will) | Topic: `stat/status`, payload: `"0"`, QoS 1. Published by broker on unexpected disconnect. |
| Clean Session | true (no persistent session — re-subscribe on every connect) |
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

### 2.6 TinyGS Website vs MQTT Control

The TinyGS website (tinygs.com) provides operator controls that are NOT available via MQTT:

| Setting | Where controlled | MQTT field? |
|---------|-----------------|-------------|
| Auto-tune on/off | Website (Operator settings via weblogin URL) | No — `remote_tune` in ESP32 config |
| Station location | Website can override; station sends in `welcome` | `station_location` in welcome, `set_pos_prm` from server |
| Station name | Website or `set_name` command | Yes — `cmnd/set_name` |
| Satellite assignment | Server-controlled when auto-tune ON | `cmnd/begine` |
| Frequency offset | Server or local | `cmnd/foff` |

**Location behavior is not fully understood.** The station sends its location in the `welcome`
payload. The server sends `set_pos_prm` back on each connect (usually `[null]`). The exact
mechanism for how location is set and persisted on the server side is unclear — it may
require the weblogin URL or other means not available via MQTT alone.

### 2.4 Startup Sequence (observed from live server)

The following messages are exchanged immediately after MQTT CONNACK:

1. **Station → Server:** `SUBSCRIBE` to `tinygs/global/#` and `tinygs/{user}/{station}/cmnd/#`
2. **Station → Server:** `PUBLISH` `tele/welcome` with station info
3. **Server → Station:** `cmnd/set_pos_prm` with payload `[null]` — **always sent on connect**, even when the station's position is already known. Safe to log and ignore when payload is `[null]`. When the server has position data (from a previous `welcome` message or website override), the payload is a JSON array: `[lat, lon, alt]` or `[alt]`.
   - **Location flow:** The station sends its location in the `welcome` payload (`station_location` field). The TinyGS website can also override the location manually. On each connect, the server sends `set_pos_prm` to push the authoritative location back to the station (which may differ from what the station sent if the user edited it on the website). If no location has been set, the server sends `[null]`.
4. **Server → Station:** `cmnd/begine` — first satellite assignment with radio config (only if auto-tune is enabled)
5. **Satellite reassignment:** `cmnd/begine` sent approximately every 60s when auto-tune is active

### 2.5 Ping Timing

The TinyGS ping interval (`tele/ping`) must NOT equal `CONFIG_MQTT_KEEPALIVE`. When both the TinyGS telemetry PUBLISH and the MQTT library's internal PINGREQ fire at the same instant, the combined TCP writes over NAT64 cause an EIO (-5) disconnect ~22s later. Use `keepalive - 30s` as the ping interval to avoid this collision.

| Setting | Value | Notes |
|---------|-------|-------|
| MQTT keepalive | 90 s (was 300 s) | Shortened from 300 s to detect dead sockets after Thread-flap-induced half-open TCP. Power cost negligible — radio is boosted-RX continuous regardless. |

### Why 90 s

Over the 22 h power-run session on 2026-04-19/20 we saw ~10 MQTT EIO disconnects. Most were not caused by NAT64 timeouts (broker confirmed ≥ 600 s idle tolerance) — they were Thread-flap-induced half-open sockets. With `CONFIG_MQTT_KEEPALIVE=300` the detection pipeline was:

- PINGREQ every 270 s (`KEEPALIVE - 30`)
- Reconnect after 2 unacked PINGRESPs → up to **~9 minutes** before we notice a dead socket

During that half-open window the server is retrying begines into a black hole. Estimated ~1-2 h/day of missed server assignment at the old rate.

At 90 s keepalive the window shrinks to ~2-3 min. Radio is in boosted-RX continuous anyway (~5.9 mA SX1262), so the extra pings add < 0.5 mW average — rounding error against the 13-16 mA system draw.
| TinyGS ping | 270s | = keepalive - 30s, avoids PINGREQ collision |
| MQTT PINGREQ | Not sent | TinyGS ping at 270s resets MQTT library's last_activity timer before keepalive threshold |

### 2.2 Published Topics (Telemetry)

| Topic | When | Purpose |
|-------|------|---------|
| `tinygs/{user}/{station}/tele/welcome` | On connect/restart | Device info, version, config |
| `tinygs/{user}/{station}/tele/ping` | Every keepalive-30s | Heartbeat with battery, memory, RSSI |
| `tinygs/{user}/{station}/tele/rx` | On packet receive | LoRa/FSK received packet (base64) |
| `tinygs/{user}/{station}/tele/get_adv_prm` | On request | Advanced parameters response |
| `tinygs/{user}/{station}/tele/get_weblogin` | On request | Web login URL |

### 2.3 Command Topics (23 total, reverse-engineered from ESP32 source)

> **Note:** This list is reverse-engineered and may be incomplete. The server may
> send commands not listed here, or some commands may be deprecated.

**Global commands** (`tinygs/global/cmnd/`):
- `batch_conf` — Batch radio configuration (same format as `begine`)
- `update` — OTA firmware update URL
- `reset` — Hard reset device
- `status` — Request status report (station responds on `stat/status`)
- `log` — Server log message (payload: string to display)
- `weblogin` — Web login URL response (payload: one-time URL)

**Station commands** (`tinygs/{user}/{station}/cmnd/`):
- `begine` — Load modem config (JSON with freq, sf, bw, cr, sat, NORAD, tlx, filter, etc.)
- `beginp` — Persist modem config to flash (same format as begine, also saves as startup config)
- `begin_lora` — Quick LoRa config (array: `[freq, bw, sf, cr, syncWord, power, currentLimit, preambleLength, gain]`)
- `begin_fsk` — Quick FSK config (array: `[freq, bitrate, freqDev, rxBw, power, preambleLength, ook, packetLength]`)
- `freq` — Set frequency (payload: float MHz as string)
- `sat` — Select satellite (payload: JSON string with name)
- `set_pos_prm` — Set station coordinates (payload: `[lat, lon, alt]`, `[alt]`, or `[null]`)
- `set_name` — Rename station (payload: `["MAC","new_name"]`, apply only if MAC matches)
- `foff` — Frequency offset in Hz (payload: float as string)
- `filter` — Packet filter (payload: JSON array of bytes)
- `tx` — Transmit data
- `sleep` — Deep sleep (payload: duration)
- `siesta` — Light sleep (payload: duration)
- `set_adv_prm` — Set advanced parameters
- `get_adv_prm` — Get advanced parameters
- `sat_pos_oled` — Satellite position for world map display (payload: `[x, y]` pixel coords)
- `frame/{num}` — Display frame data

## 3. JSON Payload Formats

### 3.1 Welcome (`tele/welcome`)

Fields and their exact types as serialized by ESP32 ArduinoJson:

| Field | JSON type | C++ source type | Example | Notes |
|-------|-----------|-----------------|---------|-------|
| station_location | array[float,float] | float | [-33.87, 151.21] | [lat, lon] |
| version | number | uint32_t | 2603242 | YYMMDDR format (NOT a string). Changing this may trigger server resets — the server may flag unknown versions for update. |
| git_version | string | const char* | "abc1234" | Git commit hash |
| chip | string | const char* | "ESP32-D0WDQ6" | From `ESP.getChipModel()`. Known values: "ESP32-D0WDQ6", "ESP32-PICO-D4", "ESP32-S3", "nRF52840". Server may send reset if unrecognized. |
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
| RSSI | number | int32_t | -45 | WiFi RSSI (ESP32) / Thread parent RSSI (nRF52) |
| radio | number | int16_t | 0 | RadioLib error code (0=OK) |
| InstRSSI | number | float | -65.0 | ESP32: unused. nRF52: Thread parent RSSI (same as RSSI). Do NOT call radio->getRSSI() during active RX — crashes SX1262 SPI. |

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
| frequency_offset | number | float | 0 | Hz — from foff command, NOT hardcoded 0 |
| satellite | string | char* | "NORBI" | Satellite name |
| sf | number | uint8_t | 10 | Spreading factor (LoRa only) |
| cr | number | uint8_t | 5 | Coding rate (LoRa only) |
| bw | number | float | 250.0 | Bandwidth kHz (LoRa only) |
| iIQ | **boolean** | bool | false | Inverted IQ (LoRa only) |
| bitrate | number | float | 9.6 | Bitrate in **kbps** (FSK only). Echoed from begine `br` — same units. |
| freqdev | number | float | 5.0 | Freq deviation in **kHz** (FSK only). Echoed from begine `fd`. |
| rxBw | number | float | 25.0 | RX bandwidth in **kHz** (FSK only). Echoed from begine `bw`. |
| data_raw | string | char* | "AQID..." | Base64 raw bits (FSK only) |
| rssi | number | float | -120.5 | Packet RSSI |
| snr | number | float | -5.25 | Packet SNR |
| frequency_error | number | float | 1234.5 | Hz |
| unix_GS_time | number | time_t | 1700000000 | Unix epoch seconds at reception (NOT uptime — must be SNTP-synced) |
| usec_time | number | int64_t | 1700000000123456 | **Microseconds since Unix epoch**, NOT uptime microseconds. The server uses this sub-second timestamp to look up the satellite's TLE position at reception; a bogus value puts the sat on the opposite side of Earth and the website shows a ~10 000 km "record distance". |
| crc_error | **boolean** | bool | false/true | CRC check result — true for CRC error packets |
| data | string | char* | "base64..." | Base64 encoded packet. For CRC errors: base64("Error_CRC") |
| NORAD | number | uint32_t | 46494 | NORAD catalog number |
| noisy | **boolean** | bool | false/true | Set true for CRC error packets (matches ESP32 behavior) |
| f_doppler | number | float | -1234.5 | Doppler correction in Hz (optional, present when Doppler compensation active) |

The RX payload field set switches based on the `mode` field: LoRa packets include sf/cr/bw/iIQ, while FSK packets include bitrate/freqdev/rxBw/data_raw instead.

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

**CRC error handling:** When the radio reports a CRC mismatch:
- If `filter[0] == 0` (no filter): send packet with `"crc_error":true`, `"noisy":true`,
  and `"data"` set to base64("Error_CRC"). The server accepts these and shows them on the website.
- If `filter[0] != 0` (filter active): silently drop CRC error packets (matches ESP32 behavior).

**FSK mode** (additional/different fields):
```json
{
  "mode": "FSK",
  "bitrate": 9.6,
  "freqdev": 5.0,
  "rxBw": 25.0,
  "data_raw": "base64_raw_bits"
}
```

All three numeric FSK fields are in **k-units** (kbps / kHz / kHz) in both
directions — `begine`'s `br`/`fd`/`bw` come in that way and are echoed
back unchanged. Fractional values are common (`br: 1.2` for narrow-band
AX.25 sats, `fd: 0.8`, `bw: 4.8`).

### 3.4 Status (`stat/status`)

Published in response to `cmnd/status`. Contains station location, version, board, tx status,
current modem configuration, and last packet metrics (rssi, snr, frequency_error, crc_error, timestamps).

The modem configuration fields vary by mode:
- **LoRa mode:** frequency, satellite, sf, cr, bw, iIQ
- **FSK mode:** frequency, satellite, bitrate, freqdev, rxBw, OOK

### 3.5 begine/batch_conf Fields

The `begine` (and `batch_conf`) command carries radio configuration. Key fields:

| Field | Type | Example | Notes |
|-------|------|---------|-------|
| mode | string | "LoRa" | |
| freq | float | 400.265 | MHz |
| bw | float | 125 | kHz |
| sf | int | 10 | 5-12 |
| cr | int | 5 | 5-8 |
| sw | int | 18 | Sync word (default 18) |
| pwr | int | 5 | TX power |
| cl | int | 120 | **Server-side only.** Content length for web display/packet decoding. The ESP32 firmware does NOT read this field. Always 120 in observed begines. Do not use for radio configuration. Sent by the TinyGS server, not by ground stations. |
| len | int | 0 | **Implicit header payload length.** 0 or absent = explicit header (default). >0 = implicit header with this fixed length. Also used as FSK fixed packet length. |
| pl | int | 9 | Preamble length |
| gain | int | 0 | LNA gain |
| crc | bool | true | CRC enable |
| fldro | int | 1 | Force LDRO |
| iIQ | bool | false | Inverted IQ (optional, defaults false) |
| sat | string | "Tianqi" | Satellite name |
| NORAD | int | 57795 | NORAD catalog number |
| filter | int[] | [1,0,235] | Packet filter bytes (optional) |
| br | **float** | 1.2 | FSK bitrate in **kbps**. Can be fractional (SAMSAT uses 1.2, Colibri-S uses 9.6). Passed unscaled to RadioLib's `beginFSK()`. |
| fd | **float** | 0.8 | FSK freq deviation in **kHz**. Can be fractional (0.8 on narrow-band AX.25 sats). |
| ook | int | 0 or 255 | FSK OOK mode (255 = OOK, 0 = GFSK). SX126x family has no OOK demodulator — stations on SX1262/SX1268 cannot correctly receive OOK sats. SX1276/1277/1278/1279 can. |
| fsw | int[] | [254,132,219] | FSK sync-word bytes (1–8 bytes). Handled as a separate array — not part of the structured descriptor. |
| len | int | 254 | For FSK: fixed packet length in bytes (set via `fixedPacketLengthMode`). For LoRa: see row above. |
| enc | int | 0/1/2 | FSK encoding: 0=NRZ, 1=Manchester, 2=data whitening (with `ws` seed). |
| ws | int | 256 | FSK whitening seed (only used when `enc==2`). |
| fr | int | 0/1/2/3 | FSK framing: 0=raw, 1=AX.25 NRZS, 2=PN9 scrambling, 3=scrambled AX.25 (x17+x12 descrambler then NRZ-S then HDLC unstuffing). |
| cSw | bool | true | Software-CRC enable. When true, radio HW CRC is disabled and the station validates CRC after decoding. |
| cB, cI, cP, cF, cRI, cRO | int / bool | 2 / 65535 / 4129 / 0 / true / true | Software CRC params: byte count, init, polynomial, final-XOR, reflect input, reflect output. |
| **tle** | string | "base64..." | **Binary TLE, 34 bytes, base64-encoded — active Doppler compensation (FSK only).** |
| **tlx** | string | "base64..." | **Binary TLE, 34 bytes, base64-encoded — position/map display only, no Doppler.** |

**TLE field naming:** The server sends TLE data under two different field names:
- `"tle"` — active Doppler compensation. The station should decode the TLE and apply real-time Doppler frequency correction. **Only sent for FSK satellites** — narrow-band FSK is sensitive to Doppler drift, so the server computes the correction.
- `"tlx"` — passive TLE. The station stores the TLE for position display but does NOT apply Doppler correction. Sent for LoRa satellites and any FSK satellite the server doesn't need to track. LoRa's chirp-spread modulation tolerates Doppler within the channel bandwidth, so no correction is needed.

Both are base64-encoded 34-byte binary representations of the satellite's two-line element set. The decoded bytes are fed to a Plan13 (P13) satellite propagator.

**Source:** Confirmed by TinyGS maintainer Stefan/OE6ISP in the tinyGS Community Telegram (Technical problems topic, 2026-04-05): *"The doppler is only calculated for the fsk-birds, for lora it is 0 - not necessary to track."* The same discussion notes that `freqOffset` (the station-wide frequency calibration, often `-99.00`) is derived from reference satellites and matters only for FSK reception.

### 3.6 Critical Radio Configuration from begine

When applying a `begine` config, these settings are REQUIRED for packet reception
(all confirmed by comparing ESP32 Radio.cpp source):

| Field | API call | Notes |
|-------|----------|-------|
| fldro | `forceLDRO(fldro)` or `autoLDRO()` when fldro==2 | **CRITICAL** — without LDRO, low-rate packets are undecodable |
| len | `implicitHeader(len)` when len>0, else `explicitHeader()` | **CRITICAL** — ESP32 uses `"len"` NOT `"cl"`. The ESP32 does not read `"cl"` at all. Most satellites omit `"len"` → explicit header (receiver reads length from LoRa header). Using `"cl"` for implicit header causes 100% CRC failure because the radio reads too many bytes. |
| — | `setRxBoostedGainMode(true)` | ~3dB better sensitivity on SX1262, always enable |
| gain | Not used on SX1262 (fixed gain) | ESP32 passes it to begin() for SX127x compat |
| iIQ | `invertIQ(iIQ)` | Must also store in radio state for RX payload |

### 3.7 Doppler Compensation

Triggered only when a `begine` payload includes a `"tle"` field (active). A `"tlx"`
payload stores the TLE for position-map display but does **not** drive Doppler —
see §3.5 for the FSK-vs-LoRa rule. Steps when `tle` is present:
1. Decode the 34-byte binary TLE from base64
2. Sync time via SNTP (Google NTP IPv6: `2001:4860:4806:8::`)
3. Compute satellite position/velocity using Plan13 propagator every 4 seconds
4. Apply Doppler frequency correction with 1200 Hz hysteresis (only retune radio when delta exceeds threshold)
5. Radio frequency = base_freq + freq_offset(foff) + doppler_correction

### 3.8 Station Command Payload Formats

These are the incoming commands the station must understand. All parse-side
behavior is tested in `tests/json_parser/`.

| Command | Payload | Action |
|---------|---------|--------|
| `set_pos_prm` | `[lat, lon, alt]`, `[alt]`, or `[null]` | Overwrite station coordinates (all three → persist to NVS; single-element → altitude only; null → no-op, server has no stored position for us). |
| `set_name` | `["MAC","new_name"]` | Rename station **only if MAC matches our 12-char client ID**. |
| `foff` | `"1500.0"` or `[offset, tolerance, refresh_ms]` | Frequency offset in Hz. Array form also tunes the Doppler hysteresis threshold and update cadence (default 1200 Hz / 4000 ms). |
| `filter` | `[count, offset, b0, b1, ...]` | Packet byte filter. `count` = number of bytes to match; `offset` = start byte in packet; `b0..bN` = expected bytes. CRC-error packets that don't match are silently dropped. |
| `tx` | raw bytes | Transmit the payload on current radio config. Our station advertises `tx:false` so this isn't sent normally, but the handler is implemented defensively. |
| `sleep` | `"60"` or `[60]` or `[60, pin]` | Deep sleep for N seconds. Station puts radio in `sleep()`, disconnects MQTT, blocks the main thread, and cold-reboots on wake. Optional `pin` (interrupt wakeup) is ignored — we only support timer wake. |
| `siesta` | same as `sleep` | Shorter "light sleep". Our implementation treats it identically to `sleep`. |
| `set_adv_prm` | JSON blob (opaque string) | Server pushes advanced radio parameters. Station stores verbatim in `cfg_adv_prm` for echo-back. |
| `get_adv_prm` | any | Station publishes `{"adv_prm":"<stored>"}` to `tele/get_adv_prm`. |
| `remoteTune` | bare number in Hz | Supplemental freq offset the server applies in addition to our TLE-computed Doppler. Stored in `freq_offset` and added to base frequency on next retune. Observed values so far: `0` (keep-alive). |
| `reset` | any | Graceful MQTT disconnect then `sys_reboot()`. |
| `status` | any | Station publishes a `stat/status` report with current modem config + last-packet metrics. |
| `log` | string | Server log message — echoed to local log, no action. |
| `weblogin` | URL string | One-time web-login URL for station configuration on tinygs.com. Logged so the user can click it. |
| `update` | URL string | OTA update URL. Not supported on UF2-bootloader hardware (there's no network-OTA path into flash); payload is logged for awareness. |
| `sat_pos_oled` | `[x, y]` | Satellite pixel position for the station's display (128×64 coords). Applied to the on-device world map. |
| `frame/{num}` | byte blob | Remote display frame data for compositing custom OLED frames. |
| `begin_lora` | `[freq, bw, sf, cr, sw, pwr, cl, pl, gain]` | Legacy array-format LoRa config. Same result as a `begine` with `mode:"LoRa"`. |
| `begin_fsk` | `[freq, br, fd, rxBw, pwr, pl, ook, len]` | Legacy array-format FSK config. Same units as the JSON form (`br` kbps, `fd` kHz, `rxBw` kHz). |

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

| Operation | Interval | Notes |
|-----------|----------|-------|
| MQTT keepalive | 90 s (was 300 s, configurable) | 600s caused watchdog timeout; 300s was reliable on the wire; 90s for fast half-open detection after Thread flaps (see §"Why 90 s") |
| TinyGS ping | keepalive - 30s | Offset avoids PINGREQ collision |
| MQTT PINGRESP | Never sent | The TinyGS ping PUBLISH at keepalive-30s resets the MQTT library's `last_activity` timer, so `mqtt_live()` never reaches the keepalive threshold to send PINGREQ. This is correct MQTT behavior — any outbound activity counts as a keepalive. The broker was confirmed to respond to PINGREQ when tested directly. |
| Satellite reassignment | ~60s | When auto-tune enabled |
| Doppler update | 4s | When TLE available, 1200 Hz hysteresis |
| RX queue drain | 2 packets per loop | ESP32 behavior |
| Reconnect base delay | 20s + random(10-20s) | ESP32 behavior |
| Max reconnect attempts | 6 | Then deep sleep 4h or restart (ESP32) |
| SNTP sync | Once after connect | Google NTP IPv6 via OT SNTP client |

## 7. Certificates

The ESP32 firmware embeds two root CA certificates in `certs.h`:
1. **TinyGS custom CA** — self-signed CA for mqtt.tinygs.com
2. **ISRG Root X1** — Let's Encrypt root (for fallback/CDN)

The nRF52 port embeds the TinyGS Intermediary CA in `src/tinygs_ca_cert.h`.

## 8. Critical Field Type Gotchas

These field type mismatches will cause the TinyGS server to reject data or show
the station as offline:

| Field | Correct | Wrong (breaks server) |
|-------|---------|----------------------|
| version | number `2604100` | string `"2604100"` |
| Vbat | int millivolts `4130` | float volts `4.13` |
| mode | string `"LoRa"` | number `1` |
| board | number `255` | string `"255"` |
| modem_conf | JSON-escaped string `"{}"` | raw JSON object `{}` |
| TLE field | `"tle"` (FSK, active Doppler) or `"tlx"` (LoRa/no-track, display only) — accept both | Treating either as an error |

## 9. Server-Initiated Resets

The TinyGS server sends `cmnd/reset` (payload `"1"`) to individual stations. This is
a station-specific command (not global broadcast). Observed triggers:

- **Suspected: version or chip changes in welcome payload.** Periodic resets were observed
  after changing `version` from 2604100 to 2604120 and `chip` from "nRF52840" to
  "nRF52840_QIAA" simultaneously. Reverting both stopped the resets. The exact trigger
  is unconfirmed — it could be either field, both, or something else entirely. The server
  may maintain a whitelist of known firmware versions or chip identifiers.
- **Auto-tune cycling:** The ESP32 firmware also receives reset commands — this may be
  normal server behavior for cycling stations between satellite assignments, though the
  ESP32 reconnects much faster over WiFi so users don't notice.

**Recommendation:** Keep `version` and `chip` values conservative. Test any changes to
welcome payload fields one at a time, with at least 15 minutes of soak testing to verify
the server doesn't start sending resets.

## 10. ESP32 Source Reference

Source: [github.com/G4lile0/tinyGS](https://github.com/G4lile0/tinyGS)
Key files:
- `src/Mqtt/MQTT_Client.cpp` — Connection, subscriptions, command dispatch
- `src/Mqtt/MQTT_credentials.cpp` — Credential management
- `src/Radio/Radio.cpp` — RadioLib integration, packet RX/TX
- `src/Status.h` — Telemetry data structures
- `src/ConfigManager/ConfigManager.cpp` — Settings persistence
- `tinyGS.ino` — Main loop and state machine
