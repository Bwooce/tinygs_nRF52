# `config.json` — Runtime Configuration

Runtime configuration lives in `config.json` on the device's USB Mass Storage
drive ("TinyGS Configurator", VID `2fe3:0001`). Mount the drive, edit the
file, unmount, power-cycle.

The device writes a fresh `config.json` from its current runtime values
every boot, so the file on the drive is always a live snapshot of what's
actually in effect. Editing a field and rebooting seeds that value into
NVS; NVS is then the authoritative store.

## Current format

The device-generated file looks like this (example values):

```json
{
  "station": "tinygs_nrf52_poc",
  "mqtt_user": "your_tinygs_username",
  "mqtt_pass": "your_tinygs_password",
  "lat": -33.8688,
  "lon": 151.2093,
  "alt": 50,
  "display_timeout": 30,
  "tx_enable": false
}
```

Standard JSON — no comments (RFC 8259), no trailing commas, strings in double
quotes. Edit with any text editor; if it stops parsing you lose configurability
until the next reboot, so be careful.

## Fields

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `station` | string | `"tinygs_nrf52_poc"` | Station name as it appears on the TinyGS dashboard. Must match the name you set when creating the station in the TinyGS web portal. Reflected in all MQTT topic paths (`tinygs/<user>/<station>/...`). Max 31 chars. |
| `mqtt_user` | string | (from `src/mqtt_credentials.h`) | TinyGS dashboard username (the account you log into `tinygs.com` with). Max 63 chars. |
| `mqtt_pass` | string | (from `src/mqtt_credentials.h`) | TinyGS dashboard password — same credential used for the web portal. Max 63 chars. |
| `lat` | float (°) | `-33.8688` | Station latitude in decimal degrees. Default is Sydney (placeholder, not a real station location). Range `-90.0` to `90.0`. Also settable by the server via the `set_pos_prm` MQTT command. |
| `lon` | float (°) | `151.2093` | Station longitude in decimal degrees. Range `-180.0` to `180.0`. |
| `alt` | float (m) | `0` | Station altitude in metres. Used in the welcome payload's `station_location` field. |
| `display_timeout` | int (s) | `30` | Seconds before the on-board TFT display blanks after last activity. Set to `0` to keep the display always on (higher power). |
| `tx_enable` | bool | `false` | Enable on-air transmit. **Default off** — station advertises `tx:false` in welcome, server never schedules transmits, `tx` MQTT command handler refuses. Setting to `true` is an explicit opt-in; operator is responsible for antenna, licensing, and regulatory compliance before enabling. |

## What happens on edit

1. Mount the USB drive (appears on host as `TinyGS Configurator`).
2. Edit `config.json`.
3. Unmount/eject the drive cleanly (this triggers the device to sync FATFS
   and re-read the file).
4. The device re-reads the file, pushes changed values into NVS, regenerates
   `config.json` with the current (now updated) values, and continues. Some
   fields take effect immediately (lat/lon, display_timeout); credentials
   take effect on the next MQTT reconnect; `tx_enable` takes effect on the
   next welcome the station publishes.

## Things you can't configure in `config.json`

These live in firmware defaults or the MQTT protocol layer:

- **Thread network parameters** (PAN ID, channel, network key) — these come
  from the commissioner during Thread join. See `docs/HA_OTBR_SETUP.md`.
- **Thread Joiner PSKd** (`TNYGS2026NRF` by default) — compiled in via
  `CONFIG_OPENTHREAD_JOINER_PSKD` in `prj.conf`. Change before flashing if
  you want a per-device PSKd.
- **MQTT broker host/port** (`mqtt.tinygs.com:8883`) — compiled into
  `src/main.cpp`. Not configurable; the whole station's protocol is
  specific to the TinyGS service.
- **Modem config** (`frequency`, `sf`, `bw`, `cr`, etc.) — these are
  driven by the server via `begine` / `batch_conf` commands once
  connected. Last-applied config is persisted to NVS as `modem_conf`
  and applied on next boot before MQTT connects, so the station can
  receive whatever sat it was last assigned even without network.
- **Radio band plan** — the SX1262 covers ~150 MHz to 960 MHz; the band is
  not a config option because the radio is band-agile per-frame.

## File lifecycle summary

- Created by firmware on first mount (or if the file is missing/deleted).
- Overwritten by firmware on every boot with current runtime values.
- Read by firmware on boot; changed values are imported into NVS.
- NVS is authoritative at runtime. `config.json` is the user-facing view.

If you want to reset a field to defaults: delete its line and reboot; the
firmware will write back the default value.
