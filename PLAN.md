# TinyGS Port to Heltec T114 (nRF52840) - Implementation Plan

## 1. Background & Motivation
The goal is to port the TinyGS firmware to the Heltec Mesh Node T114 (nRF52840 + SX1262 LoRa). The T114 provides exceptional low-power capabilities (~11µA deep sleep) making it ideal for a solar-powered satellite ground station. To achieve maximum power savings, we will replace the energy-hungry WiFi stack with an OpenThread (802.15.4) mesh networking stack.

## 2. Scope & Requirements
*   **Hardware:** Heltec T114 (nRF52840 MCU, 1MB Flash, 256KB RAM).
*   **Target Band:** Initially 433MHz, abstract hardware to support all SX1262 bands.
*   **Framework:** Zephyr RTOS (nRF Connect SDK) for professional-grade Thread and power management.
*   **LoRa Library:** Use `RadioLib` by implementing a custom Zephyr Hardware Abstraction Layer (HAL) to bridge its C++ API with Zephyr's native GPIO, SPI, and kernel timing APIs.
*   **Networking:** Native MQTT over TLS directly to `mqtt.tinygs.com` via Thread NAT64.
*   **Power:** Maximum power savings, integrating solar charging support.
*   **Configuration:** Pure USB Mass Storage (MSC) for browser-based, app-free setup.
*   **Protocol Compatibility:** Emulate an ESP32 TinyGS node perfectly to the central MQTT server. No protocol modifications.
*   **Optional:** Support for the 1.14" TFT-LCD/OLED.

## 3. Architecture & Tradeoffs

### 3.1 Networking: Direct MQTT-TLS via Thread NAT64
Unlike the ESP32 which uses WiFi, this node will act as a true standalone Thread end-device. It will connect to a standard Thread Border Router (e.g., Apple TV, HomePod) and use the Border Router's NAT64 capabilities to open a direct, persistent TLS connection (mbedTLS) to the IPv4 `mqtt.tinygs.com` server.

*   **Tradeoff - RAM vs Simplicity:** This architecture requires no local Home Assistant or third-party bridge for the user to configure. However, running a persistent TLS connection on a 256KB RAM device is exceptionally tight. 
*   **Tradeoff - Power vs Latency:** Because the TinyGS protocol does not support "next pass" ahead-of-time notification via a side channel, the node cannot completely shut down its radio. It must maintain the TCP connection (Option 1: Persistent Connection) to listen for commands and keep the cloud load balancer happy. We will rely on Thread's native Sleepy End Device (SED) polling mechanics to keep baseline power as low as possible while the TCP socket remains "open".

### 3.2 RAM Budget Analysis (256KB Total)
To fit the full stack (Zephyr + OpenThread + mbedTLS + RadioLib) into 256KB RAM without dropping TLS fragment sizes (which might break compatibility with `mqtt.tinygs.com`), we must meticulously budget memory:

| Component | Estimated RAM | Notes |
| :--- | :--- | :--- |
| **Zephyr Kernel & Stacks** | ~25 KB | Requires tuning `CONFIG_MAIN_STACK_SIZE` via Thread Analyzer. |
| **OpenThread Stack (MTD)** | ~70 KB | Configured strictly as a Minimal Thread Device (SED), no FTD routing. |
| **mbedTLS Buffers** | ~32 KB | 16KB RX + 16KB TX to support full-size TLS 1.2 fragments without dropping. |
| **mbedTLS Heap (Handshake)** | ~50 KB | Required for RSA/ECC math during the initial connection phase. |
| **Network Buffers (net_buf)**| ~15 KB | Tuned to minimum required for MQTT keep-alives and small JSON telemetry. |
| **RadioLib & App Logic** | ~30 KB | State machine, JSON parsing, LoRa buffers. |
| **Total Estimated Peak** | **~222 KB** | **Leaves ~34 KB margin.** Hardware crypto (nRF CC310) will be used to reduce CPU load and peak RAM spikes. |

### 3.3 Configuration Interface: Pure USB Mass Storage (MSC)
To achieve the absolute lowest possible static RAM footprint, we will completely eliminate the Bluetooth (BLE) stack and the WebUSB stack. The nRF52840 will be configured purely as a USB Flash Drive.

1.  **USB Composite Device (MSC + CDC ACM):** When plugged into a PC or Mac via USB-C, the device registers as a Composite USB Device. It presents both a standard USB Flash Drive (mounting the 64KB FATFS partition at 0xE2000) and a Virtual COM Port (CDC ACM) simultaneously. All Zephyr `LOG_INF()` and `printk()` output is automatically routed to this COM port so the user can monitor the Thread connection and MQTT handshakes in real-time using a serial monitor (like PuTTY or `screen`).
2.  **The Setup Page:** The Zephyr firmware dynamically generates an `index.html` file on this drive. This file contains the device's EUI64 MAC address and an auto-generated Thread Joiner Password (PSKd).
3.  **The User Workflow:**
    *   The user opens `index.html` directly from the flash drive in their web browser.
    *   The webpage displays a clean UI showing the full suite of TinyGS parameters (LoRa frequency, Spreading Factor, OLED brightness, etc.), exactly mimicking the ESP32 configuration dashboard.
    *   The webpage immediately renders the standard **Thread Commissioning QR Code** on screen (using the embedded EUI64 and PSKd) for the user to scan with their Home Assistant app to join the network.
4.  **Saving Credentials:** When the user clicks "Save Settings" on the webpage, JavaScript generates a formatted `config.json` file containing all the settings and triggers a standard browser download. The user simply saves (or drags-and-drops) this `config.json` directly back onto the USB flash drive.
5.  **Deployment:** On the next boot, the Zephyr firmware parses `config.json` using Zephyr json.h, loads the settings into RAM, and securely connects to `mqtt.tinygs.com` over the Thread mesh.

### 3.4 Post-Commissioning Lifecycle & Remote Configuration
Once the Ground Station is deployed on the roof:
*   **Remote Control:** It acts exactly like a normal ESP32 TinyGS. When the user changes settings on the TinyGS internet dashboard, the cloud server pushes a JSON payload to the device over MQTT. The Zephyr node parses the payload, applies the settings, and overwrites the `config.json` on its internal flash drive so the changes persist across reboots.
*   **Testing:** During development, we will run a fake local MQTT server to simulate the TinyGS cloud and verify these configuration payloads before connecting to the production `mqtt.tinygs.com`.

### 3.5 Bootloader Strategy & OTA Migration Path

#### Current: Adafruit UF2 Bootloader (Phase 1)
The Heltec T114 ships with the Adafruit UF2 bootloader pre-installed. For Phase 1 prototyping, we retain it.

**CRITICAL:** The Adafruit bootloader code lives at the **top** of flash (0xF4000), NOT at 0x0.
The 0x0-0x26000 region is the MBR + SoftDevice. See AGENTS.md Section 7 for full details.

| Region | Address | End | Size | Protection |
| :--- | :--- | :--- | :--- | :--- |
| MBR + SoftDevice | 0x00000 | 0x26000 | 152KB | read-only |
| Application | 0x26000 | 0xE2000 | 752KB | code_partition |
| FATFS Storage | 0xE2000 | 0xF2000 | 64KB | tinygs_storage |
| Bootloader code | 0xF4000 | 0xFDC00 | 38KB | **read-only — DO NOT TOUCH** |
| Bootloader config | 0xFDC00 | 0xFE000 | 2KB | **read-only** |
| MBR params page | 0xFE000 | 0xFF000 | 4KB | **read-only** |
| Bootloader settings | 0xFF000 | 0x100000 | 4KB | **read-only** |

*   **Flashing:** Drag-and-drop `.uf2` files via USB mass storage, or 1200-baud reset trigger.
*   **No OTA:** The UF2 bootloader has no dual-slot swap capability. Firmware updates require USB access.
*   **MCUboot is DISABLED** (`CONFIG_BOOTLOADER_MCUBOOT=n`). Enabling it without SWD will brick the device by overwriting the UF2 bootloader.
*   **Verified:** Layout confirmed via SWD recovery flash log (flash writes at 0x0-0x25DE8 and 0xF4000-0xFD858).

#### Future: MCUboot OTA (Phase 3+)
When field OTA updates are required, the bootloader must be transitioned to MCUboot. This is a **one-time, irreversible operation** that requires SWD hardware.

**Prerequisites:**
1.  An SWD debug probe (J-Link EDU Mini ~$20, or any CMSIS-DAP probe)
2.  SWD access to the T114 board (test pads or header)

**Migration Steps:**
1.  Connect SWD probe to the T114.
2.  Erase the full flash: `nrfjprog --eraseall` (destroys UF2 bootloader).
3.  Flash MCUboot: `west flash --runner jlink` targeting the MCUboot child image.
4.  Update `prj.conf`: set `CONFIG_BOOTLOADER_MCUBOOT=y`, remove `CONFIG_FLASH_LOAD_OFFSET`/`CONFIG_FLASH_LOAD_SIZE`, re-enable `CONFIG_PM=y`.
5.  Remap `app.overlay` partitions to MCUboot layout:
    *   MCUboot: 0x00000 (48KB)
    *   Slot 0: 0x0C000 (~440KB)
    *   Slot 1: 0x7C000 (~440KB)
    *   FATFS: 0xE2000 (64KB)
    *   Settings: 0xF4000 (8KB)
    *   (Exact sizes TBD based on firmware size at that point)
6.  Flash signed application image via `west flash` or `mcumgr` over USB serial.

**After migration:**
*   Development flashing via `mcumgr` over USB serial (replaces UF2 drag-and-drop).
*   Production OTA via MQTT: download signed image to Slot 1, MCUboot swaps on reboot.
*   Rollback capability: MCUboot can revert to previous image if new firmware fails validation.

**Note:** The `adafruit-nrfutil` tool can theoretically update the bootloader over USB DFU, but this path is fragile and has previously caused bricked devices. SWD is the only recommended migration method.

### 3.5 RadioLib Zephyr Integration
RadioLib is natively designed for the Arduino ecosystem. To run it on Zephyr RTOS, we will create a custom HAL (Hardware Abstraction Layer) class inheriting from `RadioLibHal`.
*   **SPI & GPIO:** Map RadioLib's pin logic to Zephyr's Devicetree (`gpio_dt_spec`) and `spi_transceive` APIs.
*   **Interrupts:** Zephyr's `gpio_add_callback` will be used to trigger RadioLib's ISRs for LoRa packet reception via the SX1262 DIO1 pin.

## 4. Phased Implementation Plan

### Phase 1: High-Risk Prototyping — COMPLETE
All Phase 1 objectives proven:
1.  **[DONE] Thread Network:** Joiner commissioning to HA SkyConnect BR. Dataset persisted in NVS.
2.  **[DONE] MQTT-TLS over Thread:** Native TLS handshake via nat64.net public NAT64 + DNS64. Authenticated to mqtt.tinygs.com:8883 with real credentials. TLS session caching enabled.
3.  **[DONE] SX1262 LoRa Radio:** RadioLib `begin()` succeeds over SPI. Pin mapping verified against T114 v2 schematic.
4.  **[DONE] RAM Budget:** FLASH 453KB/752KB (59%), RAM 162KB/256KB (62%). Comfortable. Dedicated 40KB mbedTLS heap + 8KB system heap. LTO saves ~85KB flash.
5.  **[DONE] USB MSC + CDC ACM:** FATFS at 0xE2000, NVS at 0xF2000. 1200-baud bootloader entry working.
6.  **[OBSERVING] MQTT Connection Durability:** Logging PINGRESP intervals and disconnect events to measure NAT64/broker timeouts.

**Key technical decisions from Phase 1:**
*   CC310 hardware crypto disabled — PSA RSA verify broken, Oberon software crypto used instead.
*   `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` — legacy RSA verify path required for ECDHE-RSA-CHACHA20-POLY1305-SHA256.
*   nat64.net DNS64 for hostname resolution — returns globally-routable AAAA addresses.
*   BR requires: `firewall: false`, `br routeprf high`, ip6tables MASQUERADE for Thread ULA→public IPv6.
*   UF2 file size limit ~1MB — build/flash scripts check and refuse oversized firmware.

### Phase 2: Core TinyGS Porting — COMPLETE
1.  **[DONE] State Machine & Polling:** MQTT state machine with Thread join → DNS → TLS → MQTT → satellite tracking.
2.  **[DONE] Protocol Emulation:** 23/23 MQTT commands handled. JSON escaping for modem_conf. foff and filter implemented. FSK mode implemented (sync word, encoding, whitening, OOK, software CRC, AX.25/PN9 framing).
3.  **[DONE] Interrupt-driven LoRa RX:** Full radio param parsing (freq, sf, bw, cr, sw, pl, iIQ, crc) + packet filter. Explicit header mode (ESP32 uses "len" not "cl" for implicit/explicit). **First confirmed satellite packet received 2026-04-16: Tianqi-33, server CONFIRMED.**
4.  **[DONE] NVS Config Persistence:** Zephyr Settings on shared NVS partition. config.json bidirectional sync.
5.  **[DONE] Doppler Compensation:** P13 propagator ported from ESP32. TLE parsing from begine (tle=active Doppler, tlx=passive position only). SNTP time sync via OT SNTP client (Google NTP IPv6). 4s update interval, 1200 Hz hysteresis. Satellite position computed for map display regardless of Doppler mode.
6.  **[DONE] Battery Voltage (ADC):** Real mV readings in welcome/ping/display.
7.  **[DONE] FATFS:** 64KB partition, auto-format, LFN, corruption detection.
8.  **[DONE] Auto-tune:** Weblogin URL for server-side toggle. Satellites assigned ~1/min.
9.  **[DONE] RAM Optimization:** 162KB used (62%) — down from 255KB (97%). 8KB system + 40KB mbedTLS heaps.
10. **[DONE] MQTT Stability:** TinyGS ping offset 30s before MQTT keepalive to avoid collision. TLS IN buffer 8192 for large server records. All MQTT TX buffers right-sized with overflow warnings.

### Phase 3: Peripherals & Polish — IN PROGRESS
1.  **Display (Optional):** ST7789V 240x135 TFT on SPI0. Custom 8x16 bitmap font renderer.
    - **[DONE]** Hardware init, graceful headless mode, backlight control
    - **[DONE]** Multi-page module with 3 color-coded pages cycling every 5s
    - **[DONE]** Custom 8x16 font renderer (4.6KB flash vs 114KB LVGL)
    - **[DONE]** Auto-off after 30s, wake on BOOT button press (GPIO interrupt)
    - **[DONE]** World map bitmap (240x135, scaled from ESP32 128x64 XBM) with station dot
    - **[DONE]** Satellite position dot from sat_pos_oled MQTT command
    - **[DONE]** 8 pages matching ESP32 layout including last-packet page and remote frames
    - **[DONE]** Display wakes on LoRa packet reception
    - **[DONE]** Configurable timeout via config.json `display_timeout`
2.  **LEDs:**
    - **[DONE]** Green LED (P1.03) — breathing pulse via nRFx PWM0 with timer callback for 25s pause. 5% max brightness (~0.012mA average).
    - **[DONE]** NeoPixel RGB LEDs — WS2812 via SPI2 with dummy SCK (P0.09). I2S driver broken on nRF52840, GPIO driver nRF51-only.
    - **[DONE]** Boot splash — TinyGS logo (2x scaled XBM) + version for 2s at startup.
3.  **[DONE] Hardware Watchdog:** WDT0, 600s timeout (2x keepalive). Fed on CONNACK, PINGRESP, and any MQTT RX.
4.  **[DONE] Debug Safety Checks:** CONFIG_STACK_SENTINEL + CONFIG_FORTIFY_SOURCE_RUN_TIME enabled.
    - **[TODO]** Before production: measure flash/RAM impact of removing STACK_SENTINEL, FORTIFY_SOURCE_RUN_TIME, SYS_HEAP_RUNTIME_STATS, and THREAD_NAME. Keep or remove based on cost vs safety.
5.  **Power Saving Review:** Before Phase 3 is complete, do a full power audit:
    - Measure current draw in each state (Thread join, MQTT connected idle, LoRa RX, deep sleep)
    - Identify and disable unnecessary peripherals (QSPI, unused GPIOs, USB when not needed)
    - Profile SED poll period vs. power draw
    - Target: <1mA average active, <50uA Thread idle, <11uA deep sleep
    - Implement SED latency toggling (per nrf-thread-switch pattern)
6.  **Firmware Updates:** USB-only via UF2 drag-and-drop. OTA over Thread dropped (see Section 6.3).
7.  **Commissioning Mode:** Extended wake window (15-30 min) for unprovisioned devices.
8.  **[DONE] set_name persistence:** Saves to NVS, reboots to reconnect with new station name on all topics.
9.  **[DONE] Weblogin:** Trigger via BOOT button press (rate-limited to 10s). Server responds on cmnd/weblogin with URL.
10. **MQTT command implementation status (24/24 implemented, zero stubs):**
    - **Implemented:** begine, batch_conf, beginp, begin_lora, begin_fsk, freq, sat, set_pos_prm, set_name (NVS+reboot), status, reset, tx (radio->transmit + RX resume), log, foff, filter, update (URL logged — no UF2 network-OTA path exists), weblogin, sat_pos_oled, frame/{num}, sleep, siesta, set_adv_prm (stored in cfg_adv_prm), get_adv_prm (publishes to tele/get_adv_prm), remoteTune (freq offset).
    - **Hardware-limited:** OOK mode (`ook:255`) — SX126x family has no OOK demodulator; only SX127x stations can receive OOK. Our T114 is SX1262, so we log the begine but physically can't decode OOK signals.
11. **[DONE] JSON parsing migrated to ArduinoJson.** Zephyr `json.h` descriptor-based parsing was too strict — a single unknown key or type mismatch returned `-EINVAL` and discarded the whole begine. Two production bugs today (`tle` key, fractional `br`) came from exactly that class of failure. Switch cost: +3.4 KB flash, 0 BSS. Parser is now bounded (2 KB max payload, NestingLimit(5), overflow check) so a hostile payload can't exhaust the heap. Still use `snprintf` for outbound payloads. set_pos_prm / set_name / filter / foff / fsw stay on lightweight hand-rolled parsers — forgiving by construction.
12. **[DONE] Ztest framework:** 197 unit tests on native_sim. Covers begine (incl. SAMSAT/Colibri-S real payloads, tle key, fractional br, bounds), set_pos_prm, set_name, filter, foff, fsw, sleep, adv_prm payload build, tinygs_json_escape corner cases, PN9 descrambler, AX.25 header layout.
13. **[DONE] Commissioning mode:** Detects unprovisioned device via `otDatasetIsCommissioned()`. Keeps display on 15 min, logs Joiner PSKd.
14. **[DONE] Periodic status log:** Every 5 min: uptime, connection time, MQTT/LoRa RX counts, heap used/free/max, stack_free, vbat, satellite.
15. **[DONE] TX support:** `radio->transmit()` wired to the `tx` MQTT command, followed by `startReceive()` to resume. Our welcome advertises `tx:false` so the server doesn't schedule transmissions, but the handler is there if one arrives. Actual on-air use requires the operator's licensing + antenna.
16. **[DONE] Power quick wins:** DCDC converter (board Kconfig), PM_DEVICE for SPI sleep states. 32kHz crystal reverted to RC (needs verification).
17. **[DONE] DTS-driven hardware:** Radio type, chip name, GPIO pins, radioChip enum all derived from DTS overlay. Changing board only requires overlay edit.
18. **[DONE] Crash investigation and stability fixes:**
    - **Server resets:** TinyGS server sends `cmnd/reset` when welcome fields change (chip name suffix, version number). Keep `version` and `chip` conservative.
    - **getRSSI crash:** `radio->getRSSI()` during active LoRa RX corrupts SX1262 SPI bus. ESP32 uses `WiFi.RSSI()` for ping, never radio RSSI. Fixed: use Thread parent RSSI for InstRSSI field.
    - **Watchdog timeout:** Zephyr MQTT library doesn't auto-disconnect on missed PINGRESPs (`unacked_ping` just increments). Fixed: feed watchdog on any MQTT RX, reduce keepalive from 600s to 300s.
    - **Retained-RAM crash diagnostic:** `__noinit` variables survive warm reset, log PC/LR/ICSR (→ IRQ number/name) and faulting thread name on next boot. RESETREAS fully decoded (PIN/DOG/SREQ/LOCKUP). Reason code resolved to `CPU_EXCEPTION`/`SPURIOUS_IRQ`/`STACK_CHK_FAIL`/…
    - **Stack usage:** 3824 B peak on main / 8 KB allocated (53% headroom). Log-processing thread bumped to 2 KB for text formatter scratch.
    - **PWM0 spurious IRQ:** nrfx_pwm when used directly (not via Zephyr's pwm driver) does not auto-connect its IRQ. First breathing-LED sequence completion triggered a NULL-vector IRQ → `K_ERR_SPURIOUS_IRQ` → reboot loop. Fixed by `irq_connect_dynamic(PWM0_IRQn, 6, nrfx_pwm_0_irq_handler, …)` + `CONFIG_DYNAMIC_INTERRUPTS=y`.
    - **TCXO define ordering:** `LORA_DTS_NODE` was referenced before its own `#define DT_ALIAS(lora0)` in `tinygs_protocol.h`. The `DT_NODE_HAS_PROP` check evaluated against an unset node and the TCXO macro silently fell to 0.0 V. `init_radio` hardcoded 1.8 so LoRa worked, but the FSK begin path passed the macro and the SX1262 rejected mode changes with -707. Fixed by reordering.
    - **usec_time units:** Was uptime-microseconds; server uses it as Unix-epoch microseconds for TLE position lookup. Wrong value placed our station on the opposite side of Earth ("Record distance: 10 000 km" on the website). Now `get_utc_epoch_us()` returns `k_uptime_get() * 1000 + sntp_offset * 1_000_000`.
    - **modem_conf buffer:** Widened 256 → 512 B and tle buffer 34 → 64 B to match ESP32 and survive future protocol extensions. NVS self-heal drops oversized entries left over from prior builds.
19. **[TODO] SED mode + power gating:** Requires on-site testing (see Phase 4).

### Phase 4: Power Optimization & Commissioning
Requires current measurement equipment and on-site testing.

1.  **Commissioning mode:** Extended wake window (15-30 min) for unprovisioned devices.
    Device must stay fully awake (no SED) during Thread joining and DTLS commissioning handshake.
    Once provisioned and connected, transition to power-saving mode.
2.  **SED (Sleepy End Device) mode:**
    - Enable CONFIG_OPENTHREAD_MTD_SED + CONFIG_OPENTHREAD_POLL_PERIOD=60000 (60s)
    - Implement poll period toggling: fast (100-500ms) during MQTT activity, slow (60s) idle
    - Set otThreadSetChildTimeout to 4x poll period
    - TCP/TLS survives over SED — packets buffered at parent router
3.  **SX1262 duty cycle RX:** Use startReceiveDutyCycle() for hardware-managed RX/sleep cycling
    instead of continuous RX. Saves ~4.1mA (4.6mA → 0.5mA).
4.  **Peripheral power gating:**
    - Vext (P0.21) LOW when LEDs/GPS not needed (saves ~3mA)
    - TFT_EN (P0.03) LOW when display blanked (saves ~1.5mA)
    - usb_disable() when no USB cable detected (saves ~1mA)
5.  **Current measurement:** Baseline each state with a power profiler
    - Thread joining, MQTT connected idle, LoRa RX, deep sleep
    - Target: <1mA average (estimated achievable based on research)
6.  **CONFIG_RAM_POWER_DOWN_LIBRARY=y** — power down unused SRAM banks

### Phase 5: RadioLib ZephyrHal Upstream PR
The Zephyr HAL is functionally complete and multi-instance safe. To submit as a PR
to [jgromes/RadioLib](https://github.com/jgromes/RadioLib), the following packaging
work is needed (no existing RadioLib HAL has formal tests — the bar is a working
example + clean HAL source):

1.  **Create `examples/NonArduino/Zephyr/` directory** with:
    - `main.cpp` — standalone SX1262 TX/RX example (~70 lines, matching RPi example style)
    - `CMakeLists.txt` — Zephyr-style cmake (find_package(Zephyr), target_sources)
    - `prj.conf` — minimal Zephyr config (SPI, GPIO, logging)
    - `app.overlay` — DTS overlay with SPI + GPIO pin bindings for a reference board
    - `README.md` — build instructions, tested hardware, pin mapping
2.  **Move HAL source into RadioLib tree** — `src/hal/Zephyr/ZephyrHal.h` and `.cpp`
    following the existing `RPi/PiHal.h` naming convention.
3.  **Remove on-target smoke tests** from the HAL source (test_ZephyrHal.cpp stays in
    our app, not in the RadioLib PR). The example serves as the functional test.
4.  **Test on a second radio module** (SX1278 or SX1276) to validate generality beyond
    SX1262. At minimum, confirm `begin()` succeeds and a register read returns expected
    values.
5.  **Test on a second Zephyr board** (e.g., nRF52840 DK or nRF5340 DK) to confirm
    the HAL isn't T114-specific. The DTS overlay should be board-agnostic.
6.  **Review RadioLib contribution guidelines** and open a PR with:
    - Description of the HAL and what it enables (Zephyr RTOS support)
    - List of tested hardware + Zephyr/NCS versions
    - Note that it's been running in production on a TinyGS ground station

## 5. Limitations & Requirements for Users
*   **Infrastructure:** Requires the user to have a standard Thread Border Router (e.g., Apple TV 4K, HomePod Mini, or Home Assistant SkyConnect) on their network.

## 6. Architectural Risks & Constraints

### 6.1 Persistent TLS over Thread SED — MEDIUM RISK (downgraded)
*   **The Problem:** Maintaining a persistent TCP/TLS connection for MQTT over a Thread Sleepy End Device (SED).
*   **Measured Results (2026-04-11):**
    - **300s keepalive: WORKS** — confirmed with 2+ consecutive PINGRESPs via nat64.net NAT64
    - **600s keepalive: WORKS** — confirmed with PINGRESP at 600s connected
    - NAT64 conntrack timeout is > 600s for established TCP connections
    - TinyGS broker does not drop idle connections at 600s
    - CONFIG_MQTT_KEEPALIVE=300 currently deployed (reverted from 600s for reliability)
*   **Risk downgraded:** The original concern was NAT64 timeouts at 120-300s. Actual measured tolerance is >600s, giving 10x fewer wakeups than the feared 60s minimum.
*   **Mitigations:**
    1.  ~~**Measure during Phase 1:**~~ **DONE** — 600s confirmed working.
    2.  **Connect/disconnect pattern:** Still useful for deep sleep. Connect → publish → subscribe → wait for commands → disconnect → deep sleep for N minutes. See `fgervais/project-nrf-thread-switch` for SED latency toggling reference pattern.
    3.  **TLS session resumption:** Enable `CONFIG_MBEDTLS_SSL_SESSION_TICKETS` to cache session state. Reconnection avoids the expensive full handshake (~50KB heap spike).
    4.  ~~**Investigate MQTT 5.0 session expiry**~~ — mqtt.tinygs.com uses MQTT 3.1.1 only.

### 6.2 USB MSC + FATFS Concurrent Access — MEDIUM RISK
*   **The Problem:** USB MSC is a block-level protocol. The host OS assumes exclusive control over FAT sectors when mounted. Writing to FATFS from firmware while the host has the drive mounted will corrupt the filesystem.
*   **Current Status:** The code writes `index.html` at boot (before USB enumeration), which is safe. The risk surfaces when MQTT config changes need to be persisted back to `config.json` at runtime.
*   **Mitigations:**
    1.  Write files only at boot, before `usb_enable()`.
    2.  For runtime config persistence, use NVS (separate partition) and sync to FATFS only on reboot.
    3.  Never write to FATFS while USB MSC is active.

### 6.3 OTA Firmware Updates — DROPPED (USB-only)
*   **Decision:** OTA over Thread is impractical for a solar-powered device. 440KB over 802.15.4 (~10-20 KB/s best case, much worse for SED) requires minutes of continuous radio-on time.
*   **Approach:** Firmware updates are USB-only via UF2 drag-and-drop. Users with physical access flash new firmware directly. A small MQTT notification can alert users when updates are available.
*   **Future option:** If OTA is revisited, use CoAP block transfer (not MQTT) over Thread, or require the user to bring the device within USB range.

### 6.4 mbedTLS Memory — RESOLVED (was HIGH RISK)
*   **The Problem:** Static RAM usage was 241KB / 256KB (92%) before any TLS handshake heap allocation.
*   **Resolution:** Aggressive optimization reduced RAM to 162KB (62%):
    - System heap: 80KB → 8KB (peak measured: 516 bytes)
    - mbedTLS heap: 60KB → 40KB (dedicated, required for handshake fragmentation)
    - LTO: CONFIG_LTO=y + CONFIG_ISR_TABLES_LOCAL_DECLARATION=y saves ~85KB flash
    - CC310 hardware crypto disabled (PSA RSA verify broken). Oberon software crypto used.
*   **Current TLS buffer config:**
    - CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=8192 (IN=8192, OUT=4096)
    - Heap sharing not used — mbedTLS needs contiguous heap for cert parsing
*   **MQTT TX/RX buffers right-sized:**
    - mqtt_tx_buf: 128 (MQTT header+topic only; payload sent via iovec)
    - mqtt_rx_buf: 256 (streaming read buffer)
    - payload_buf: 512 (outbound JSON, with truncation warnings)
    - rx_payload: 768 (inbound server payloads)

### 6.5 RadioLib on Zephyr — RESOLVED (was LOW RISK)
*   **Resolution:** RadioLib ZephyrHal fully working. Multi-instance safe (CONTAINER_OF pattern). SX1262 init, config, TX, RX all proven. Interrupt-driven packet reception via DIO1 GPIO callback.
*   **Actual linked size:** ~1.3KB after LTO (pre-LTO object was 160KB — misleading).

### 6.6 NVS/FATFS Storage Conflict — RESOLVED
*   **The Problem:** OpenThread stores persistent credentials via Zephyr's NVS settings subsystem. FATFS stores config.json/index.html. Sharing a partition would corrupt both.
*   **Resolution:** Split into two partitions: FATFS 64KB (0xE2000-0xF2000), NVS 8KB (0xF2000-0xF4000). FATFS needs ≥128 sectors (64KB at 512B/sector) for FatFs auto-format. FATFS is unmounted before `usb_enable()` to prevent concurrent access corruption. Raw flash signature check (0x55AA) prevents FatFs from crashing on corrupted data.

### 6.7 Flash Wearout & Filesystem Durability — HIGH RISK
*   **The Problem:** FATFS in the 64KB partition does not perform hardware wear-leveling (unlike SD cards). The nRF52840 flash is rated for ~10,000 erase cycles.
*   **The Risk:** If remote MQTT config changes frequently overwrite `config.json`, the same 6 flash pages will be repeatedly erased, potentially destroying them in a matter of months.
*   **Mitigations:**
    1.  Strictly limit runtime FATFS writes.
    2.  For incoming MQTT config updates, save them to the **NVS partition** (which does wear-level). Only regenerate the physical `config.json` on the FATFS partition during a system reboot or when the user explicitly triggers USB MSC mode.

### 6.8 Thread Commissioning Timeout vs. Deep Sleep — MEDIUM RISK
*   **The Problem:** Users must plug into USB, read `index.html` for the QR code, then physically deploy the node before scanning it in Home Assistant.
*   **The Risk:** If the firmware enters its 11µA deep sleep (SED) mode too quickly, the Thread radio will turn off. By the time the user walks outside to scan the code, the Border Router won't be able to reach the device for the DTLS commissioning handshake.
*   **Mitigations:** Implement a dedicated **"Commissioning Mode"** state. If the device is unprovisioned, it must remain fully awake (Radio RX on) for a generous window (e.g., 15-30 minutes) after boot before falling back to the aggressive SED sleep cycle.

### 6.9 Stack Overflows — LOW RISK (was HIGH)
*   **The Problem:** Stack overflows in Zephyr cause silent hard faults.
*   **Resolution:** RAM is now 162KB (62%), leaving ~94KB free. Risk substantially reduced.
*   **Active mitigations:**
    - CONFIG_STACK_SENTINEL=y — writes sentinel word at bottom of each thread stack, checked on context switch
    - CONFIG_FORTIFY_SOURCE_RUN_TIME=y — catches buffer overflows in libc calls (memcpy, snprintf, etc.)
    - All MQTT payload buffers have overflow/truncation warnings
*   **[TODO] Production:** Assess flash/RAM cost of removing STACK_SENTINEL, FORTIFY_SOURCE_RUN_TIME, SYS_HEAP_RUNTIME_STATS, and THREAD_NAME. Keep or remove based on savings vs safety tradeoff.

### 6.10 Hardware Crypto (CC310) — RESOLVED (was HIGH RISK)
*   **Resolution:** CC310 hardware crypto disabled. PSA RSA PKCS1v15 verify returns INVALID_ARGUMENT via CC310 driver. Using Oberon software crypto instead. CONFIG_MBEDTLS_USE_PSA_CRYPTO=n forces legacy (non-PSA) path for TLS. OpenThread uses PSA directly (unaffected). RAM budget is comfortable without CC310.