# AI Agent Instructions - TinyGS nRF52 Port

This document provides foundational mandates and workflows for AI agents working on the TinyGS port to the Heltec Mesh Node T114 (nRF52840).

## 1. Project Mandates
- **Framework:** Must use Zephyr RTOS (nRF Connect SDK).
- **Core Strategy:** Native MQTT over TLS directly to `mqtt.tinygs.com` via OpenThread NAT64.
- **Hardware:** Heltec T114 (nRF52840 + SX1262 LoRa).
- **Architecture:** Zero Bluetooth/Matter. Use Pure USB Mass Storage Class (MSC) for initial configuration.

## 2. Development Workflow
- **Environment:** The Zephyr/NCS workspace is isolated in `./ncs`. The Python virtual environment is in `./.venv`.
- **Build:** Use `./build.sh`. It is optimized for 16 cores (`CMAKE_BUILD_PARALLEL_LEVEL=16`).
- **Flash:** Use `./flash.sh`. It triggers a 1200-baud auto-reset, waits for the UF2 bootloader drive to mount, and copies the firmware.
- **Debugging:** All logs are routed to the USB CDC ACM serial port (`/dev/ttyACM0`). Always use `LOG_INF`, `LOG_ERR`, etc. Use `python3 scripts/serial_log.py` to monitor and log to file.

### USB Device Identities
The device presents different USB identities depending on its state:

| State | VID:PID | USB Name | /dev/sda is | /dev/ttyACM0 is |
|-------|---------|----------|-------------|-----------------|
| **UF2 Bootloader** (double-tap RST or 1200-baud reset) | 239a:0071 | HT-n5262 | UF2 flash drive — copy .uf2 here | Bootloader serial |
| **Application firmware** (normal boot) | 2fe3:0001 | TinyGS Configurator | 24KB FATFS partition (config.json) — NOT for flashing | CDC ACM console log |

**IMPORTANT:** When the application is running, `/dev/sda` is the FATFS config partition, NOT the bootloader flash drive. Do NOT copy .uf2 files to it.

### Flash Workflow
1. `./build.sh` — builds firmware, generates `build/zephyr/zephyr.uf2`
2. `./flash.sh` — sends 1200-baud reset, mounts bootloader drive, copies .uf2
3. Device auto-reboots into new firmware
4. `python3 scripts/serial_log.py /dev/ttyACM0 115200 serial.log` — monitor output

### 1200-Baud Reset (Verified Working)
The firmware registers a CDC ACM baud rate callback. When the host sets 1200 baud
(via `stty -F /dev/ttyACM0 1200`), the firmware writes `0x57` to GPREGRET and does
a cold reboot, entering the UF2 bootloader. This avoids needing a physical RST double-tap.
Note: the LOG_INF in the callback was intentionally removed — logging to USB from USB
IRQ context would deadlock.

## 3. Memory Management (The "Squeeze" Playbook)
The nRF52840 has 256KB RAM. The current baseline (Thread + USB MSC + mbedTLS) uses ~230KB. If RAM becomes critical (< 5KB free), apply these optimizations in order:

1.  **LTO:** Enable `CONFIG_LTO=y` in `prj.conf` to strip unused static data.
2.  **Heap Sharing:** Set `CONFIG_MBEDTLS_ENABLE_HEAP=n` to force mbedTLS to use the global system heap (`CONFIG_HEAP_MEM_POOL_SIZE`) instead of a static 60KB array.
3.  **Stack Profiling:** Use `CONFIG_THREAD_ANALYZER=y` to identify and shrink oversized thread stacks.
4.  **Buffer Shrinking:** As a last resort, reduce `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` from 16384 to 4096 (requires server-side fragment support).

## 4. Coding Standards
- **RadioLib HAL:** All LoRa interaction must go through the custom `ZephyrHal` class in `src/hal/`.
- **JSON:** Use the `ArduinoJson` library (provided in `lib/ArduinoJson`) for all MQTT and config parsing.
- **Portability:**
    - Use explicit byte shifting for wire protocols.
    - Always cast `uint32_t` to `(unsigned long)` and use `%lu` or `%lX` in log statements for compatibility across ESP32/nRF52.
    - Every Zephyr thread/workqueue must feed the Task Watchdog if enabled.

## 5. Configuration Architecture
- Do NOT add BLE or Matter code.
- Configuration is handled via a dynamic `index.html` on the USB MSC drive.
- The device parses `config.json` from the FATFS partition at boot.
- Remote configuration changes via MQTT must be persisted back to the `config.json` file.

## 6. OTA Updates
- OTA is deferred to Phase 3+ and will require MCUboot (flashed via SWD).
- For Phase 1, use the Adafruit UF2 bootloader with `.uf2` files for USB flashing.
- See PLAN.md Section 3.5 for the MCUboot migration path.

## 7. Flash Partition Safety — CRITICAL

### The Adafruit UF2 bootloader lives at the TOP of flash, NOT at 0x0.

The nRF52840 flash layout with Adafruit bootloader (verified via SWD recovery log):

```
Address    Region                      Size     Protection
────────────────────────────────────────────────────────────
0x00000    MBR + SoftDevice S140       152KB    read-only (DTS)
0x26000    Application code            792KB    code_partition
0xEC000    FATFS storage (USB MSC)     24KB     tinygs_storage
0xF2000    NVS settings (OpenThread)   8KB      storage_partition
0xF4000    Adafruit Bootloader code    38KB     read-only (DTS) ← PROTECTED
0xFDC00    Bootloader config           2KB      ← PROTECTED
0xFE000    MBR params page             4KB      ← PROTECTED
0xFF000    Bootloader settings         4KB      ← PROTECTED
0x100000   End of flash
```

### MANDATORY RULES — violating these BRICKS the device:

1. **NEVER place any writable partition at or above 0xF4000.** The bootloader, its
   config, MBR params, and settings pages occupy 0xF4000-0x100000. Writing to ANY
   address in this range destroys the bootloader and requires SWD to recover.

2. **NEVER set CONFIG_FLASH_LOAD_SIZE such that FLASH_LOAD_OFFSET + FLASH_LOAD_SIZE > 0xEC000.**
   The application must not extend into the FATFS region or beyond.

3. **NEVER move any writable partition above 0xEC000** except NVS at 0xF2000-0xF4000.
   FATFS (24KB) must stay within 0xEC000-0xF2000. NVS (8KB) within 0xF2000-0xF4000.

4. **Always verify the partition map after ANY change to app.overlay or prj.conf:**
   - Build the project
   - Check `build/zephyr/zephyr.dts` for the merged partition layout
   - Confirm NO partition overlaps with 0xF4000-0x100000
   - Confirm the UF2 start address in build output matches 0x26000

5. **The `boot_partition` at 0x0 is the MBR + SoftDevice, NOT the bootloader.**
   The actual bootloader code is `bootloader_partition` at 0xF4000. Both must be
   marked `read-only` in the DTS overlay.

6. **CONFIG_BOOTLOADER_MCUBOOT must remain `n`.** Enabling it generates MCUboot
   child images that overwrite the UF2 bootloader when flashed via UF2.

### How the bootloader was bricked (twice):
- A FATFS partition was placed at 0xF8000 (inside the bootloader region)
- `fs_mkfs()` erased flash pages from 0xF8000 to 0x100000
- This destroyed the bootloader binary, config, MBR params, and settings
- Device became completely unresponsive on USB (no bootloader = no DFU)
- Recovery required SWD probe + full reflash of bootloader + SoftDevice

### Reference:
- Adafruit bootloader linker: https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/linker/nrf52840.ld
- Bootloader code: FLASH ORIGIN=0xF4000, LENGTH=38KB
- MBR params: 0xFE000 (4KB)
- Bootloader settings: 0xFF000 (4KB)

## 8. Runtime Configuration Items

These values need to be user-configurable at runtime (eventually via NVS Preferences
store with wear-levelling). Items marked **[server]** are set/updated by the TinyGS
MQTT server. Items marked **[user]** are set locally via USB MSC config.json or
commissioning. Items marked **[build]** are compile-time only (prj.conf).

### Station Identity
| Item | Source | Current Location | Notes |
|------|--------|-----------------|-------|
| MQTT username | **[user]** | mqtt_credentials.h (gitignored) | TinyGS dashboard credential |
| MQTT password | **[user]** | mqtt_credentials.h (gitignored) | TinyGS dashboard credential |
| Station name | **[user]** | Derived from FICR DEVICEID | MAC-based %04X%08X |
| Station latitude | **[user]** | config.json `lat` field, tinygs_station_lat | Read at boot from FATFS; default -33.8688 (Sydney) |
| Station longitude | **[user]** | config.json `lon` field, tinygs_station_lon | Read at boot from FATFS; default 151.2093 (Sydney) |
| Station altitude (m) | **[user/server]** | config.json `alt` field, tinygs_station_alt | Read at boot; also updated by set_pos_prm command |

### Radio Configuration (from server)
| Item | Source | Current Location | Notes |
|------|--------|-----------------|-------|
| Frequency (MHz) | **[server]** | Hardcoded 436.703 | Via begine/batch_conf MQTT command |
| Spreading factor | **[server]** | Hardcoded 10 | 7-12 |
| Coding rate | **[server]** | Hardcoded 5 | 5-8 |
| Bandwidth (kHz) | **[server]** | Hardcoded 250.0 | |
| Satellite name | **[server]** | tinygs_radio.satellite | Via begine/batch_conf/sat commands |
| NORAD ID | **[server]** | tinygs_radio.norad | Catalog number from server |
| Freq offset (Hz) | **[server]** | Not stored | Via foff command (TODO) |
| Sync word | **[server]** | Default 18 | |
| CRC settings | **[server]** | Defaults | sw CRC, poly, init, etc. |
| Packet filter | **[server]** | Not stored | Via filter command (TODO) |
| modem_conf | **[server]** | Hardcoded "{}" | Last begine/batch_conf JSON payload; echoed in welcome |

### Operational Settings
| Item | Source | Current Location | Notes |
|------|--------|-----------------|-------|
| Station name | **[server]** | MQTT_CLIENT_ID (hardcoded) | Via set_name command; needs NVS persist + reconnect |
| MQTT keepalive (s) | **[build]** | prj.conf CONFIG_MQTT_KEEPALIVE=600 | Also sets TinyGS ping interval; 600s tested |
| TX allowed | **[user]** | Hardcoded false | Currently always false |
| Low power mode | **[user]** | Not implemented | Phase 3 SED sleep config |
| OT log level | **[build]** | prj.conf OPENTHREAD_LOG_LEVEL_CRIT | CRIT/WARN/NOTE/INFO/DEBG |
| App log level | **[build]** | prj.conf LOG_DEFAULT_LEVEL=3 | 0=off, 1=err, 2=wrn, 3=inf, 4=dbg |

### Thread Network (managed by OpenThread)
| Item | Source | Current Location | Notes |
|------|--------|-----------------|-------|
| Thread dataset | **[auto]** | NVS (0xF2000) | Obtained via Joiner commissioning |
| Joiner PSKd | **[build]** | prj.conf OPENTHREAD_JOINER_PSKD | "TNYGS2026NRF" |
