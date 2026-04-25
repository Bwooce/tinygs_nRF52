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
*   **Tradeoff - Power vs Latency:** Because the TinyGS protocol does not support "next pass" ahead-of-time notification via a side channel, the node cannot completely shut down its radio. It must maintain the TCP connection (Option 1: Persistent Connection) to listen for commands and keep the cloud load balancer happy. The node therefore stays a full-rx Thread MTD (not SED — see §20 audit); Thread-level power savings are unavailable as long as MQTT keepalive is pinned, so the primary power lever is SX1262 hardware duty-cycle RX rather than the Thread poll period.

### 3.2 RAM Budget Analysis (256KB Total)
To fit the full stack (Zephyr + OpenThread + mbedTLS + RadioLib) into 256KB RAM without dropping TLS fragment sizes (which might break compatibility with `mqtt.tinygs.com`), we must meticulously budget memory:

| Component | Estimated RAM | Notes |
| :--- | :--- | :--- |
| **Zephyr Kernel & Stacks** | ~25 KB | Requires tuning `CONFIG_MAIN_STACK_SIZE` via Thread Analyzer. |
| **OpenThread Stack (MTD)** | ~70 KB | Configured as a full-rx Minimal Thread Device, no FTD routing. SED rejected — see §20. |
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
| FATFS Storage | 0xE2000 | 0xF2000 | 64KB | tinygs_storage (config.json) |
| NVS Settings | 0xF2000 | 0xF4000 | 8KB | storage_partition (OpenThread + tgs/*) |
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
    *   Settings: 0xF2000 (8KB)
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

### 3.6 Radio Layering vs. Multi-Chip Support
Three separate layers — worth keeping distinct because they answer different
questions and live in different places:

| Layer | What it does | Where it lives | What varies per chip |
|-------|-------------|----------------|----------------------|
| Application | begine dispatch, Doppler, post-RX framing, filter, CRC | `src/main.cpp`, `src/tinygs_protocol.*` | calls differ per chip |
| **Radio abstraction** (missing today) | uniform virtual interface across SX chip families | *would live in* `src/` | normalises per-chip API gaps |
| RadioLib | per-chip classes (`SX1262`, `SX1276`, `SX1268`, `SX1280`, `LR1121` …) | `lib/RadioLib/` submodule | **every method signature differs slightly** |
| Platform HAL (`ZephyrHal`) | GPIO / SPI / IRQ / delay for RadioLib | `src/hal/` | board pin map only |

The **ESP32 TinyGS project has this abstraction layer** — see `RadioHal<T>` in
`tinyGS/src/Radio/RadioHal.hpp`. It's a C++ template that wraps each RadioLib
chip class into a common virtual interface so the application can call e.g.
`radioHal->beginFSK(...)` regardless of which chip is fitted. Per-chip quirks
(`setWhitening` existing on SX1268 but not SX1262, different `beginFSK`
signatures, `autoLDRO` shape, etc.) live inside the template specialisations.

**We don't have an equivalent** because we only target SX1262 today — a single
concrete chip doesn't need a virtual base. `main.cpp` calls RadioLib directly.
The day we add a second chip (likely SX1268 for a Wio-family board, or SX1276
for a Feather), we should port a minimal `RadioHal<T>`-equivalent into `src/`
(not into `lib/RadioLib/` — RadioLib's philosophy is to expose each chip's full
capability, not a lowest-common-denominator wrapper). Adding it would be a
~200-line refactor; doing it speculatively before a second target exists is
dead weight. Cross-reference Phase 5 §4 (ZephyrHal multi-chip validation) —
those two items pair naturally when a second chip enters the picture.

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
    - Target: <1mA average. Deep-sleep targets don't apply — MQTT keepalive is pinned (§20).
    - Primary lever: SX1262 `startReceiveDutyCycle()` hardware RX gating.
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
    - **Watchdog timeout:** Zephyr MQTT library doesn't auto-disconnect on missed PINGRESPs (`unacked_ping` just increments). Fixed: feed watchdog on any MQTT RX, then reduced keepalive progressively: 600s → 300s (PINGRESP reliability over NAT64), then 300s → 90s (dead-socket detection after Thread-flap-induced half-open TCP — ~9 min worst case at 300s, ~2-3 min at 90s; power cost negligible on always-on boosted-RX radio).
    - **Retained-RAM crash diagnostic:** `__noinit` variables survive warm reset, log PC/LR/ICSR (→ IRQ number/name) and faulting thread name on next boot. RESETREAS fully decoded (PIN/DOG/SREQ/LOCKUP). Reason code resolved to `CPU_EXCEPTION`/`SPURIOUS_IRQ`/`STACK_CHK_FAIL`/…
    - **Stack usage:** 3824 B peak on main / 8 KB allocated (53% headroom). Log-processing thread bumped to 2 KB for text formatter scratch.
    - **PWM0 spurious IRQ:** nrfx_pwm when used directly (not via Zephyr's pwm driver) does not auto-connect its IRQ. First breathing-LED sequence completion triggered a NULL-vector IRQ → `K_ERR_SPURIOUS_IRQ` → reboot loop. Fixed by `irq_connect_dynamic(PWM0_IRQn, 6, nrfx_pwm_0_irq_handler, …)` + `CONFIG_DYNAMIC_INTERRUPTS=y`.
    - **TCXO define ordering:** `LORA_DTS_NODE` was referenced before its own `#define DT_ALIAS(lora0)` in `tinygs_protocol.h`. The `DT_NODE_HAS_PROP` check evaluated against an unset node and the TCXO macro silently fell to 0.0 V. `init_radio` hardcoded 1.8 so LoRa worked, but the FSK begin path passed the macro and the SX1262 rejected mode changes with -707. Fixed by reordering.
    - **usec_time units:** Was uptime-microseconds; server uses it as Unix-epoch microseconds for TLE position lookup. Wrong value placed our station on the opposite side of Earth ("Record distance: 10 000 km" on the website). Now `get_utc_epoch_us()` returns `k_uptime_get() * 1000 + sntp_offset * 1_000_000`.
    - **modem_conf buffer:** Widened 256 → 512 B and tle buffer 34 → 64 B to match ESP32 and survive future protocol extensions. NVS self-heal drops oversized entries left over from prior builds.
19. **[PARTIAL] Peripheral power gating + SX1262 duty-cycle RX:** Phase 4 work, on-bench measurement gates the rest. SED dropped, see §20. Sub-status:
    - **[DONE] USB stack gated by VBUS detect** (commit `a058cca`, refactored to system workqueue in `393266f`). `usb_vbus_work` runs every 250 ms with 2 s debounce, toggling `usb_enable`/`usb_disable` so HFXO releases on battery. ~1 mA expected; awaits power-probe confirmation.
    - **[TODO] SX1262 startReceiveDutyCycleAuto** — design + helper pattern in §4.2 above. ~1 day with hardware once a probe is wired up. Biggest single power lever (~4 mA).
    - **[TODO] Vext gating** — audit in §4.3 above. ~3 mA. Soak-test items: NeoPixel re-init after Vext cycle, bootloader DFU LED behaviour.
    - **[DONE] CONFIG_RAM_POWER_DOWN_LIBRARY=y** + capped picolibc malloc
      arena to 4 KB so it can't grow into the powered-down section.
      Bonus finding from STATUS log instrumentation (`pmem` field):
      picolibc malloc is *literally unused* — `pmem=0(peak=0/4096)` on
      a healthy device. C++ `new` resolves to k_malloc/sys_heap, not
      picolibc malloc, so RadioLib allocations don't touch the arena.
      The 4 KB cap is therefore wildly over-provisioned — could be
      shrunk to 1 KB or even 0 to recover RAM (trivial but not worth
      the touch right now). Filed as a future micro-optimisation.
    - **[BLOCKED on measurement] TFT_EN gating** — see §4.3 above; SLPIN already drops the panel ~150 µA; whether cutting the rail saves an additional ~1.3 mA or just ~150 µA is unknown without a current probe. Driver-side re-init complexity isn't worth committing to before the measurement.

20. **Thread protocol feature audit (2026-04-18):** Investigated what Thread 1.2/1.3 features might improve reliability and power for our use case. Conclusions:
    - **[DONE] MLR (Multicast Listener Registration)** — `CONFIG_OPENTHREAD_MLR=y`. Without it, iot_log multicast (ff05::e510) reaches us only via the BBR's default-flood policy, which loses coherence when the mesh link weakens. MLR makes the BBR hold an explicit forwarding entry for our device with periodic refresh. Directly addresses the "moved device outside, logs went quiet while MQTT kept working" failure mode.
    - **Child Supervision** — compiled in by default on OpenThread 1.3+ (the `OPENTHREAD_CONFIG_CHILD_SUPERVISION_ENABLE` knob was removed upstream; see `ncs/modules/lib/openthread/src/core/config/openthread-core-config-check.h:648`). Defaults in `child_supervision.h`: **INTERVAL=129 s** (parent→child keepalive cadence), **CHECK_TIMEOUT=190 s** (child re-attaches if silence exceeds this). For an outdoor weak-link ground station these defaults are reasonable — re-attach in ≤3.5 min, ~14 keepalive frames/hr per child. Shorter interval would burn more radio time without a clear benefit since MQTT already has its own 90 s PINGREQ that triggers reconnection higher up the stack. Not tuned.
    - **SED (Sleepy End Device) — rejected, permanently.** Our architecture pins MQTT keepalive open so the MTD radio has to stay on to receive PINGRESPs and server-initiated commands; SED would just trade 802.15.4 RX for polled 802.15.4 RX with no net saving. Tearing MQTT down to enable SED sleep windows is worse: TLS re-handshake costs ~8 KB transient RAM + several seconds during which a satellite pass could be missed. The real power lever for this product is SX1262 hardware duty-cycle RX (see Phase 4 §2), not the Thread stack.
    - **CSL (Coordinated Sampled Listening) — rejected, tied to SED.** Same logic.
    - **Link Metrics — deferred.** Useful only if we have a consumer (e.g. prefer different parent, publish to tinygs). Without a plan, it's instrumentation without action.
    - **DUA (Domain Unicast Address) — rejected.** DUA gives a stable globally-routable IPv6 but no transport — we'd still need an HTTP server + TLS + cert management to expose anything useful, duplicating tinygs.com's existing per-station dashboard.
    - **Device web UI (like ESP32's IotWebConf) — accepted, see §21.** Original rejection cited ~60-80 KB RAM cost, which assumed on-device TLS termination. Revisiting with a plain-HTTP-over-Thread-mesh baseline (Thread's network key already encrypts at L2) drops the cost to ~10 KB RAM / 20 KB flash — comfortable against the current 91 KB RAM / 277 KB flash headroom. mTLS remains an optional upgrade.
    - **Commissioner on device — rejected.** HA's OTBR is already the commissioner; a local one duplicates work without simplifying joining.
    - **TREL (Thread Radio Encapsulation Link) — not applicable.** TREL tunnels Thread over Wi-Fi/Ethernet for multi-radio mesh peers. We're 802.15.4-only and single-radio; no second interface to tunnel over.
    - **Network Diagnostics (`OT_TMF_NETWORK_DIAGNOSTIC`) — already available.** OT cores include the TMF diagnostic handler on MTDs. We can already be polled by the BR for route/parent/link-quality TLVs without a new Kconfig. If we ever want to *initiate* diagnostics from the device, that's a client-side addition; no consumer today.
    - **SRP client (service registration) — already enabled** via `CONFIG_OPENTHREAD_SRP_CLIENT=y`. Unused at present (we don't advertise any mDNS/SRP service). Opportunity: publish a `_tinygs._tcp` record so HA can discover the device on the Thread-side address without hardcoding. Deferred until we have an HA integration that consumes it.
    - **DNS client (`OPENTHREAD_DNS_CLIENT=y`) — already enabled** and used for `mqtt.tinygs.com` resolution via the BR's NAT64 DNS proxy. Working.
    - **Ping sender (`OPENTHREAD_PING_SENDER`) — intentionally off** (`=n`) to save flash; diagnostic only.
    - **`ot-cli` over the CDC console — rejected.** Adds ~12 KB flash and a shell thread for a feature we can already exercise via the HA OTBR (`ot-ctl`). Not worth the footprint.
    - **Native NAT64 (`OPENTHREAD_NAT64_TRANSLATOR`) — rejected.** Only meaningful on a BR; as an MTD we consume NAT64 via the HA BR and just need a valid route (`fd14:db8:3::/96`), which we already have.
    - **Link Quality Indicator logging — minor win.** `otThreadGetParentInfo()` already surfaces parent RSSI/LQI and we already publish parent RSSI in the MQTT InstRSSI field. Periodic diagnostic log of LQI transitions would help correlate outdoor-link failures; cheap to add but not urgent.

21. **Device Web UI — direct port of the ESP32 IotWebConf + dashboard (design 2026-04-18):**

    **Goal:** reachability from a phone/laptop on the same Thread mesh for live diagnostics and configuration, mirroring what a user gets when they browse to an ESP32 TinyGS station on their home Wi-Fi. Target parity with the ESP32 UI; don't reinvent the UX.

    **Transport baseline (plain HTTP over Thread):** device listens on `[thread-mesh-address]:80`. The Thread network key already encrypts 802.15.4 at L2 inside the home, and the MTD is not reachable from the public internet (NAT64 is outbound-only). HTTP Basic auth for endpoints behind the `admin` user matches what the ESP32 already does. No device-side TLS, no cert management. Users reach the UI from HA-side hosts that have a route into the Thread mesh, or directly from phones joined to the Thread commissioning flow. Same threat model as an ESP32 station running HTTP over home Wi-Fi: fine for a trusted LAN, exposes credentials to anyone already on the same segment.

    **Interaction with the HA OTBR firewall — the real reason to care about TLS:** the HA OpenThread Border Router add-on ships with an `ip6tables`-based firewall option (off by default in many installs) that, when enabled, blocks LAN→Thread ULA unicast ingress. This is the right setting for protecting *other* Thread devices on the mesh — in particular Matter devices, which otherwise sit open to anyone on the LAN. Turning the firewall on with a plain-HTTP web UI has a consequence: the web UI stops being reachable from LAN, because LAN→ULA ingress is exactly what the firewall blocks. Three coherent end-states:

    1. **Firewall OFF + plain HTTP** (what this design assumes by default): simplest, ESP32-parity security, Matter devices also LAN-reachable. Accepts LAN-level trust.
    2. **Firewall ON + port-scoped `ip6tables` allow rule**: add a rule allowing LAN → `[our-device-ula]:80` specifically. Matter devices protected; our web UI works; other stations on the same Thread mesh still unreachable from LAN unless they get their own rule.
    3. **Firewall ON + device-side mTLS (§21.5)**: device listens on 443 with a locally-generated CA. The firewall doesn't need a hole — standard OT BR forwarding treats the mesh→HA route as trusted, and mTLS gates access on the client cert. The cleanest option if you plan to run the OTBR firewall long-term; also the only option that protects the web UI credential path at LAN level.

    **Optional upgrade (§21.5):** swap in mTLS with a locally-generated CA if *either* the threat model expands (untrusted-LAN scenarios) *or* you want the OTBR firewall on without punching per-port holes. mbedTLS is already linked for the MQTT client; the per-device server-side cost is ~4-8 KB RAM.

    **Scope — what we port (matches ESP32 `ConfigManager.cpp`):**

    | ESP32 endpoint | Purpose | Port decision |
    |---|---|---|
    | `GET /` | Root page: dashboard/config/firmware/restart buttons + OTP code | **Port** — drop firmware button (USB-only) |
    | `GET /logo.png` | TinyGS logo | **Port** — same PNG from `logos.h`, ~2.5 KB |
    | `GET /config` | Station config form (lat/lon, MQTT, board, TZ, modemStartup, boardTemplate, adv_prm) | **Port** — POST handler writes NVS, triggers reboot on MQTT changes |
    | `GET /dashboard` | SVG world map + cards (GS/modem/sat/last packet) + web serial console | **Port** — same layout, feed from same `status` struct |
    | `GET /restart` | Confirm page + reboot | **Port** — `sys_reboot(SYS_REBOOT_COLD)` |
    | `GET /cs?c1=<cmd>&c2=<counter>` | Console poll + command (`!p` test packet, `!w` weblogin, `!e` reset, `!o` OTP) | **Port** — hook into our existing log ring buffer |
    | `GET /wm` | Worldmap data (CSV of sat pos + modem + GS status + sat data + last packet) | **Port** — exact same payload shape, fed from `status` |
    | `GET /firmware` | ArduinoOTA update page | **Drop** — USB UF2 only |

    **Scope — what we drop:**
    - Wi-Fi AP / captive portal — Thread replaces this; joining is handled at OTBR
    - Per-device OTP display — MQTT station credentials are provisioned via MQTT over Thread, not via the UI
    - IotWebConf AP password parameter — use a fixed `admin` Basic-auth credential stored in NVS (editable from the config form)
    - MDNS/Bonjour advertisement — deferred to the SRP-client opportunity already noted in §20

    **Zephyr implementation:**

    1. **HTTP server:** `CONFIG_HTTP_SERVER=y` (Zephyr's in-tree async HTTP/2-capable server, experimental but functional on NCS v2.x). Set `CONFIG_HTTP_SERVER_MAX_CLIENTS=4` minimum — TLS handshakes, slow phones, and overlapping `/cs`+`/wm` from multiple browser tabs can transiently consume slots even though every handler returns promptly. The +1-2 KB RAM cost is in budget. **Fallback** (which may end up being the *primary* path): a hand-rolled HTTP/1.0 dispatcher using `zsock_poll()` on a single listening socket, in one low-priority thread. The whole UI is ~8 endpoints so it's a ~200-line implementation, and a single-threaded event loop sidesteps the MAX_CLIENTS issue entirely (no fixed worker pool, just FDs in a poll set). Try the in-tree server first to save code, but don't treat the hand-rolled path as merely a safety net.
    2. **Routing:** register each endpoint with `http_service_register()`; handler signatures take a request + response buffer + status ref. Same handler-per-path shape as `server.on()` on ESP32.
    3. **Auth:** middleware decorator checks `Authorization: Basic <base64(admin:<pw>)>`. Returns 401 with `WWW-Authenticate` on miss, matching ESP32 behaviour.
    4. **Template rendering:** port the ESP32's `String s += "..."` builders to a fixed-size `snprintf`-into-response-buffer pattern. No dynamic allocation in the hot path. Static chunks (`IOTWEBCONF_HTML_HEAD`, CSS, scripts) stay as `const char[]` in flash, identical to ESP32's `PROGMEM` content.
    5. **Static content (logo, CSS, JS):** embed in flash via `include/webui_assets.h` generated from `logos.h` + `html.h`. Reusing FATFS is not attractive — host OS has the drive mounted over USB MSC, so any firmware write risks corruption (§6.2).
    6. **Config persistence:** posted form values feed into the existing NVS config path (same code that today consumes MQTT `cmnd/setconfig` payloads). `boardTemplate` and `modemStartup` stay as opaque JSON strings; `adv_prm` uses the dict-table editor script already present in ESP32's `ADVANCED_CONFIG_SCRIPT`.
    7. **Console poll (`/cs`):** **short-poll**, not long-poll. Browser sends `c2=<last-seen-counter>` every ~2.3 s; handler returns immediately with any new log lines emitted since that counter (or empty). Same shape as ESP32's `cmnd/cs`.
       - **Two log backends, not one shared buffer.** Zephyr's `LOG_MODE_DEFERRED` (default) calls every registered `log_backend` with each message, so we register two: backend A is the existing `log_backend_ringbuf` feeding CDC ACM (unchanged); backend B is a tiny new backend writing into a dedicated 4 KB ring buffer that the HTTP `/cs` handler is the sole consumer of. No multicast logic to write ourselves, no risk of `/cs` consumption draining bytes from the USB serial port, no SPSC-violation in either ring buffer.
       - `c1=` commands dispatch through the same command handlers as MQTT `cmnd/cs` (already implemented for `!p`/`!w`/`!e`/`!o`).
    8. **Worldmap poll (`/wm`):** exact CSV format as ESP32 (the dashboard's JavaScript already parses it). Fields sourced from our `status` struct + P13 propagator output.
    9. **Cross-thread access to `tinygs_radio` and `status`.** Today `main()` is the only mutator (MQTT loop, `lora_check_rx`, `doppler_update` all run in sequence in the main loop), so no locking exists. The HTTP server runs on its own thread (or in a single dispatcher thread for the hand-rolled fallback), which introduces a real read/write race: a multi-field update like `{freq=x; sf=y; bw=z}` can be observed mid-flight, and even single-field 32-bit reads of `last_rssi`/`last_snr`/`sat_pos_*` can tear if the compiler splits them. Mitigation: a single `k_mutex radio_mutex`. Mutators in `main()` lock briefly across the *update*; the HTTP handler locks briefly across a *snapshot copy*, then serialises the snapshot without the lock held:
       ```c
       k_mutex_lock(&radio_mutex, K_MSEC(50));
       struct tinygs_radio_t snap = tinygs_radio;   /* memcpy under lock */
       int8_t rssi = last_rssi; int8_t snr = last_snr;
       k_mutex_unlock(&radio_mutex);
       /* serialise 'snap'/rssi/snr to response without the lock */
       ```
       Locks held for microseconds; serialiser runs lock-free; hot RX path isn't penalised by response generation latency.

    **Budget estimate (measured against current 475 KB flash / 165 KB RAM used):**

    | Component | Flash | RAM | Notes |
    |---|---|---|---|
    | `CONFIG_HTTP_SERVER` + socket service | +12-15 KB | +4-6 KB | shared `net_buf` pool, small per-request scratch, `MAX_CLIENTS=4` |
    | Dedicated web-UI log backend + ring buffer | +0.5 KB | +4 KB | second `log_backend`; sole consumer is `/cs` handler |
    | `radio_mutex` + snapshot copies | +0.2 KB | +0 KB | one `k_mutex`; snapshot lives on response stack |
    | Embedded assets (logo, CSS, JS, HTML chunks) | +8-10 KB | ~0 | `const` in flash, flash-XIP |
    | Handler code + form parser | +3-5 KB | +1-2 KB | builder strings go on the response stack |
    | HTTP Basic auth + base64 | +1 KB | +0 KB | libc crypt is not used; base64 is `<mbedtls/base64.h>` already in build |
    | **Total (HTTP baseline)** | **~26 KB** | **~12 KB** | Leaves ~250 KB flash / ~79 KB RAM free |
    | Optional: mTLS upgrade | +2-4 KB | +4-8 KB | adds a server-side TLS context, reuses mbedTLS |

    **Reachability:**
    - From HA box on same LAN: route `fd14:db8:3::/64` (our OMR prefix) via the BR; device reachable on its mesh-local or ULA address
    - From a phone joined to the Thread mesh (iPhone 15+, Thread-capable Android): direct
    - From public internet: not reachable — that's a feature, not a bug

    **Out of scope / decided against:**
    - ArduinoOTA `/firmware` endpoint (USB UF2 only per §6.3)
    - AP mode / captive portal (Thread replaces Wi-Fi provisioning)
    - Public-internet exposure (no DynDNS, no port forward, no ACME)
    - Server-Sent Events / WebSockets for the console (polling already works at 2.3 s cadence on ESP32; no reason to change)

    **Risks:**
    - **FATFS host-OS corruption (§6.2 applies):** the UI's config POST must write NVS only, never the FATFS partition — same rule we enforce for MQTT `cmnd/setconfig`.
    - **Thread MTD RX duty while user browses:** opening the dashboard pins the radio at full RX during the session; the periodic `/wm` poll every 5 s keeps us awake. This aligns with our "no SED, MQTT pinned" architecture so it's not an incremental cost.
    - **Zephyr `http_server` maturity:** the in-tree server is marked experimental. Fallback plan is a ~200-line hand-rolled HTTP/1.0 parser on a listening socket (see implementation §1).
    - **Cross-thread torn reads:** introducing the HTTP thread breaks the current "main() is the only mutator" invariant. Without mitigation, the `/wm` poll can observe `tinygs_radio` mid-update and render inconsistent values. Mitigated by `radio_mutex` snapshot pattern (see implementation §9). Audit any new shared state before exposing it via HTTP.

    **Build phasing:**
    1. Static responses + routing skeleton (root, logo, restart). Verifies the HTTP stack on Thread.
    2. Dashboard HTML + `/wm` poll handler. Confirms the `status` struct feeds real data.
    3. Console `/cs` short-poll + dedicated log backend. Proves the dual-backend log plumbing (CDC ACM unaffected when web UI is consuming).
    4. Config form POST handler + NVS writes. Highest-risk step (must not corrupt existing MQTT config path).
    5. (Optional) mTLS upgrade if threat model changes.

### Phase 4: Power Optimization & Commissioning
Requires current measurement equipment and on-site testing.

**SED/CSL explicitly out of scope** — see §20 audit. The MTD stays a full-rx child
because MQTT keepalive must stay pinned; the Thread radio isn't where the power is.

1.  **Commissioning mode:** Extended wake window (15-30 min) for unprovisioned devices
    during Thread joining and DTLS commissioning handshake. No post-commissioning
    MTD→SED transition (see §20).
2.  **SX1262 duty cycle RX:** Use `startReceiveDutyCycleAuto()` for hardware-managed RX/sleep
    cycling instead of continuous RX. Saves ~4.1 mA (4.6 mA → 0.5 mA). This is the primary
    power lever.
    - **Why a wrapper is needed:** `startReceiveDutyCycle` and `startReceiveDutyCycleAuto`
      are SX126x-only — declared in `lib/RadioLib/src/modules/SX126x/SX126x.h:326,346`
      and not virtual on `PhysicalLayer`. An SX1276/SX1278 build would fail to compile
      if it called them.
    - **Implementation pattern:** wrap every restart site behind a `start_radio_rx()`
      helper that branches on the same DTS gate already used to type the `radio` pointer
      (`main.cpp:911-919`):
      ```c
      static void start_radio_rx(void) {
          if (!radio) return;
      #if DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1262) || \
          DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1268)
          uint16_t min_symbols = (strcmp(tinygs_radio.modem_mode, "FSK") == 0) ? 8 : 0;
          int state = radio->startReceiveDutyCycleAuto(tinygs_radio.pl, min_symbols);
          if (state != RADIOLIB_ERR_NONE) {
              LOG_WRN("DC RX failed (%d), falling back to continuous", state);
              radio->startReceive();
          }
      #else
          /* SX127x: no hardware DC. Continuous RX (CAD-poll variant possible later). */
          radio->startReceive();
      #endif
      }
      ```
      Then replace each `radio->startReceive()` call site in `main.cpp` (~12 sites: after
      packet RX in `lora_check_rx`, after TX, after doppler retune, on begine config
      change, in the radio re-init recovery path, etc.) with `start_radio_rx()`. This is
      the same DTS-gate pattern already used for every chip-specific call on `radio`, just
      hoisted into one helper.
    - **Min-symbols / preamble caveat (non-obvious):** the second arg controls how long
      the chip naps between sniffs. Pass `tinygs_radio.pl` (preamble length from the
      most recent begine) as the sender-preamble argument. TinyGS sats vary widely
      (LoRa pl=8 typical, FSK can be pl=80 for slow-data sats); using a wrong/stale
      value silently drops packets because the chip wakes too late to catch the preamble.
      Make sure the `pl` field in `tinygs_radio` is updated on every begine and that
      `start_radio_rx()` is called *after* the begine-driven `setPreambleLength()`, not
      before.
    - **Implementation easier now?** Yes — three preconditions are already in place:
      (a) `radio` is already a chip-typed pointer behind a DTS gate, so adding the
      `#if` is the same pattern as everywhere else; (b) `tinygs_radio.pl` is already
      tracked per-begine in `tinygs_protocol.cpp`; (c) the doppler-retune mid-packet
      guard from the recent fault-handler / retune commit is in place, so DC RX won't
      collide with mid-packet retunes. The work is roughly: write the helper, sed the
      ~12 call sites, soak-test against an active sat, instrument with a current probe
      to confirm the ~4 mA saving. ~1 day with hardware.
3.  **Peripheral power gating:**
    - **Vext (P0.21) LOW when not needed (saves ~3 mA).** Audit:
      Vext is currently held HIGH from `enable_peripherals()` and never
      released. The only consumer on the T114 is the on-board NeoPixel
      strip (SK6812 × 2, fed by SPI2 P0.14, supply line gated by
      Vext per app.overlay line 38). Everything else has its own
      enable: green LED is direct PWM0 (independent rail), TFT
      logic+backlight are on TFT_EN/backlight, ADC bias has its own
      ADC_CTRL pin (P0.06), no GPS module is fitted. Safe to default
      Vext OFF at boot, raise only during commissioning (visual
      feedback during Thread join / DTLS) and on packet RX flashes,
      lower again when idle. Single owner is enough — no refcount
      needed yet. Soak-test items: confirm SK6812 accepts new SPI
      frames after a Vext-off→Vext-on cycle, and that the Adafruit
      bootloader doesn't assume Vext=HIGH for its DFU LED indication.
    - **[BLOCKED on bench measurement] TFT_EN (P0.03) LOW when display
      blanked.** Originally estimated ~1.5 mA. Reality is more nuanced:
      `display_blanking_on()` already issues SLPIN to the ST7789V,
      which per datasheet drops the panel to ~150 µA. The PLAN's
      1.5 mA assumed the panel module's DC-DC / driver IC has tail
      current beyond SLPIN that disappears when the rail is cut. We
      don't actually know without a current probe.
      Implementation cost is non-trivial: cycling TFT_EN power resets
      the ST7789V controller registers (COLMOD, MADCTL, gamma, sleep
      state). The Zephyr driver's PM `RESUME` action only sends SLPOUT,
      assuming the rail stayed up. The full re-init lives in
      `st7789v_init()` which is `static` and only called once via
      `DEVICE_DT_INST_DEFINE`. Three options if we proceed:
      (a) patch the upstream driver to expose runtime re-init (best
      long-term but a PR cycle); (b) replicate ~100 lines of init
      sequence in app code (fragile); (c) accept losing the saving.
      **Decision: skip until we measure.** On the next bench session
      with a current probe, take SLPIN-only and SLPIN+rail-off readings
      back-to-back. If the delta is >0.5 mA, do option (a). Below that,
      not worth the patch.
    - **[DONE] USB stack gated by VBUS detect** — `usb_vbus_poll()` in main loop
      polls `NRF_POWER->USBREGSTATUS` every 100 ms with a 2 s debounce.
      Boot-time `usb_enable()` is skipped if no cable; hot-plug/unplug
      toggles the stack at runtime so HFXO can release on battery.
      Expected ~1 mA saving; awaits hardware measurement to confirm.
4.  **Current measurement:** Baseline each state with a power profiler
    (PPK2 or INA219 rig — see methodology block below).
    - Thread joining, MQTT connected idle, LoRa RX, individual peripherals
    - **Realistic floor target: ~8–9 mA average.** Vbat-drift method on the
      April 2026 run measured ~17–18 mA average. Confirmed-doable items
      address ~8.6 mA: SX1262 DC RX (~4), Vext gating (~3), USB disable
      (~1, [DONE]), CONFIG_RAM_POWER_DOWN (~0.3), PWM uninit when idle
      (~0.3). TFT_EN (~1.5 estimated) is parked pending a measurement —
      see §3 above; if the bench shows >0.5 mA delta it goes back into
      the budget and floor drops to ~7 mA. Remaining ~7 mA is Thread MTD
      radio rx-on-when-idle (~5–6 mA, pinned by MQTT keepalive — can't
      drop without going SED, which is rejected per §20) plus nRF52840
      active-core overhead (~1 mA). Sub-1 mA is not reachable on this
      architecture; it would require SED + MQTT teardown between passes,
      both of which trade the wrong things for our use case.
5.  **CONFIG_RAM_POWER_DOWN_LIBRARY=y** — power down unused SRAM banks

**Interim current estimate — Vbat-drift method (±20–30 %):**

Until an INA219 shunt or Nordic Power Profiler is wired in, average current is
estimated by running the station on a known-capacity LiPo for 2–4 days and
dividing the observed Vbat drop by the LiPo plateau slope. Method:

- Capture Vbat mean-of-last-5 STATUS samples at baseline (post-unplug, after a
  ~20 min settle so charger tail-off doesn't bias things).
- Run untouched for 2–4 days, mean-of-5 again at the end.
- `avg_mA ≈ ΔV / slope / run_h`, with slope ≈ 0.15–0.20 mV/mAh in the
  4.00–3.85 V LiPo plateau. Noise floor per ADC sample is ±80 mV, so mean-5
  drops σ to ~36 mV — ΔV needs to be well past that to be real signal.
- **Invalidation conditions:** any USB reconnect (= charging, resets the
  baseline), reflash, more than a handful of unexpected reboots, or >12 h
  iot_log silence gap.

**Run #2 result (full discharge, 2026-04-19 → 2026-04-25, 131 h):** 17–18 mA
average ±10%, on the deployed firmware (commit `af424e7`). Practical brownout
at 2.37 V LiPo terminal — well below datasheet VBAT, the DCDC bypass kept
the SoC running until the cell could no longer supply. 27 sessions over the
run (26 mid-run reboots) — those were almost certainly software faults; the
deployed firmware lacks the fault-handler instrumentation that would have
captured cause, so we don't know which ones. Numbers carry ±10% spread because
the cell isn't impedance-characterised. Real shunt measurement is item §4
above and will supersede.

### Phase 2.5: NCS Upgrade v2.6.0 → v3.3.0 (prerequisite for Phase 3)
Must run **before** Phase 3 web UI, not after. Full breakdown in
**[docs/NCS_UPGRADE_PLAN.md](docs/NCS_UPGRADE_PLAN.md)**. Three drivers:
- Our current Zephyr has only a stub `CONFIG_HTTP_SERVER` — the real subsystem
  Phase 3 needs landed in Zephyr v3.7 (shipped in NCS v3.3.0).
- Two OpenThread DNS-client bugfixes directly touch our NAT64 code path and
  aren't backported to the v2.6 LTS line.
- 25 months of security fixes in mbedTLS / network stack / USB.

Desk-research pass resolved the four "scary" open questions (TLS ciphersuite
still works, OT MTD timers unchanged, FATFS auto-format unchanged, http_server
is the real gain). The migration is mostly a toolchain/kconfig exercise, not an
API rewrite. Budget 7–10 days focused work. Gate on: `v0.2` tag validated on
hardware + a clean power-run baseline + a 2-week window.

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

### 6.1 Persistent TLS keepalive over Thread — MEDIUM RISK (downgraded)
*   **The Problem:** Maintaining a persistent TCP/TLS connection for MQTT over Thread to a NAT64-translated IPv4 broker.
*   **Measured Results (2026-04-11):**
    - **300s keepalive: WORKS** — confirmed with 2+ consecutive PINGRESPs via nat64.net NAT64
    - **600s keepalive: WORKS** — confirmed with PINGRESP at 600s connected
    - NAT64 conntrack timeout is > 600s for established TCP connections
    - TinyGS broker does not drop idle connections at 600s
    - CONFIG_MQTT_KEEPALIVE=90 currently deployed (300→90 for faster half-open detection after Thread flaps; 600→300 earlier for PINGRESP reliability over NAT64)
*   **Risk downgraded:** The original concern was NAT64 timeouts at 120-300s. Actual measured tolerance is >600s, giving 10x fewer wakeups than the feared 60s minimum.
*   **Mitigations:**
    1.  ~~**Measure during Phase 1:**~~ **DONE** — 600s confirmed working.
    2.  **TLS session resumption:** Enable `CONFIG_MBEDTLS_SSL_SESSION_TICKETS` to cache session state. Reconnection avoids the expensive full handshake (~50KB heap spike).
    3.  ~~**Investigate MQTT 5.0 session expiry**~~ — mqtt.tinygs.com uses MQTT 3.1.1 only.
    4.  ~~**Connect/disconnect-on-deep-sleep pattern:**~~ Dropped with SED — see §20. MQTT stays pinned.

### 6.2 USB MSC + FATFS Concurrent Access — MEDIUM RISK
*   **The Problem:** USB MSC is a block-level protocol. The host OS assumes exclusive control over FAT sectors when mounted. Writing to FATFS from firmware while the host has the drive mounted will corrupt the filesystem.
*   **Current Status:** The code writes `index.html` at boot (before USB enumeration), which is safe. The risk surfaces when MQTT config changes need to be persisted back to `config.json` at runtime.
*   **Mitigations:**
    1.  Write files only at boot, before `usb_enable()`.
    2.  For runtime config persistence, use NVS (separate partition) and sync to FATFS only on reboot.
    3.  Never write to FATFS while USB MSC is active.

### 6.3 OTA Firmware Updates — DROPPED (USB-only)
*   **Decision:** OTA over Thread is impractical for a solar-powered device. 440KB over 802.15.4 (~10-20 KB/s best case) requires minutes of continuous radio-on time even at full-rx.
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

### 6.8 Thread Commissioning Timeout — LOW RISK
*   **The Problem:** Users must plug into USB, read `index.html` for the QR code, then physically deploy the node before scanning it in Home Assistant.
*   **Risk downgraded:** The MTD runs full-rx at all times (SED rejected, see §20), so the Thread radio never sleeps — the original concern about the radio shutting off before commissioning is moot.
*   **Mitigations retained:** The Commissioning Mode state still exists to extend the window before considering a device provisioned, but it no longer gates any sleep transition.

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