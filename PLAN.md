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
5.  **Deployment:** On the next boot, the Zephyr firmware parses `config.json` using the `ArduinoJson` library, loads the settings into RAM, and securely connects to `mqtt.tinygs.com` over the Thread mesh.

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
4.  **[DONE] RAM Budget:** FLASH 464KB/752KB (62%), RAM 253KB/256KB (98%). Tight but functional. Dedicated 60KB mbedTLS heap required for cert parsing.
5.  **[DONE] USB MSC + CDC ACM:** FATFS at 0xE2000, NVS at 0xF2000. 1200-baud bootloader entry working.
6.  **[OBSERVING] MQTT Connection Durability:** Logging PINGRESP intervals and disconnect events to measure NAT64/broker timeouts.

**Key technical decisions from Phase 1:**
*   CC310 hardware crypto disabled — PSA RSA verify broken, Oberon software crypto used instead.
*   `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` — legacy RSA verify path required for ECDHE-RSA-CHACHA20-POLY1305-SHA256.
*   nat64.net DNS64 for hostname resolution — returns globally-routable AAAA addresses.
*   BR requires: `firewall: false`, `br routeprf high`, ip6tables MASQUERADE for Thread ULA→public IPv6.
*   UF2 file size limit ~1MB — build/flash scripts check and refuse oversized firmware.

### Phase 2: Core TinyGS Porting — COMPLETE
1.  **[DONE] State Machine & Polling:** MQTT state machine with Thread join → DNS → TLS → MQTT connect → subscriptions → satellite tracking.
2.  **[DONE] Protocol Emulation:** All 23 MQTT commands handled. JSON payloads match ESP32 types. Welcome, ping, RX, status messages verified. modem_conf needs JSON escaping (TODO).
3.  **[DONE] Interrupt-driven LoRa RX:** DIO1 ISR → readData → base64 → MQTT publish pipeline working. Full radio param parsing (freq, sf, bw, cr, sw, pl, iIQ, crc) from begine commands.
4.  **[DONE] NVS Config Persistence:** Zephyr Settings on shared NVS partition. Location, station, credentials loaded from NVS. config.json bidirectional sync (read at boot, regenerated from runtime values).
5.  **[DEFERRED] Power Management:** 600s keepalive confirmed working. SED latency toggling deferred to Phase 3.
6.  **[DONE] Battery Voltage (ADC):** P0.04 (AIN2) with GPIO6 bias enable. 100k:390k divider, real mV readings in welcome/ping.
7.  **[DONE] FATFS:** 64KB partition with auto-format, LFN enabled, corruption detection via boot sector signature check.
8.  **[DONE] Auto-tune:** Weblogin URL mechanism discovered and implemented. Server assigns satellites via begine commands (~1/min).
9.  **Remaining:** JSON escaping for modem_conf echo. RAM at 97% (255KB/256KB).

### Phase 3: Peripherals & Polish — IN PROGRESS
1.  **Display (Optional):** ST7789V 240x135 TFT on SPI0. DTS and driver configured.
    - **[DONE]** Hardware init, graceful headless mode, backlight control
    - **[DONE]** Multi-page module (tinygs_display.cpp) with page cycling
    - **[TODO]** Bitmap font rendering (text currently placeholder)
    - **[TODO]** 3 pages: station info, satellite tracking, system health
    - **[TODO]** Auto-off after 30s, wake on BOOT button press
    - **[TODO]** Flash on LoRa packet reception
2.  **RGB LEDs:** The T114 has two RGB LEDs. Define purpose:
    - LED1: Connection status (solid = connected, slow blink = joining, fast blink = error)
    - LED2: LoRa activity (flash on packet RX)
    - Both OFF when display is off (power saving mode), with optional slow pulse (~1/30s) as heartbeat
3.  **Power Saving Review:** Before Phase 3 is complete, do a full power audit:
    - Measure current draw in each state (Thread join, MQTT connected idle, LoRa RX, deep sleep)
    - Identify and disable unnecessary peripherals (QSPI, unused GPIOs, USB when not needed)
    - Profile SED poll period vs. power draw
    - Target: <1mA average active, <50uA Thread idle, <11uA deep sleep
    - Implement SED latency toggling (per nrf-thread-switch pattern)
4.  **Firmware Updates:** USB-only via UF2 drag-and-drop. OTA over Thread was evaluated and dropped due to power cost (see Section 6.3).
5.  **Commissioning Mode:** Extended wake window (15-30 min) for unprovisioned devices.
6.  **Hardware Watchdog:** nRF52840 has one WDT (WDT0). Feed on MQTT PINGRESP; if no PINGRESP within 2x keepalive, watchdog reboots the device.

### Phase 4: RadioLib ZephyrHal Upstream PR
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
    - CONFIG_MQTT_KEEPALIVE=600 currently deployed (configurable in prj.conf)
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

### 6.4 mbedTLS Memory — HIGH RISK
*   **The Problem:** Static RAM usage is 241KB / 256KB (92%) before any TLS handshake heap allocation. Only ~15KB margin remains.
*   **The Risk:** The 80KB system heap + 60KB mbedTLS heap = 140KB of heap pools (55% of total RAM). `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=4096` saves RAM but may fail if mqtt.tinygs.com sends >4KB TLS records. Peak ECC/RSA math during handshake could exhaust remaining heap.
*   **Measured:** Build reports 241KB static RAM (vs. plan estimate of 222KB). The 34KB margin from the plan is actually ~15KB.
*   **Mitigations (Squeeze Playbook, in order):**
    1.  **CC310 hardware crypto:** ~~Enable `CONFIG_HW_CC3XX=y`~~ DONE — `CONFIG_PSA_CRYPTO_DRIVER_CC3XX=y` enabled. +70KB flash, +640B RAM.
    2.  **Heap sharing:** Set `CONFIG_MBEDTLS_ENABLE_HEAP=n` to merge mbedTLS heap into system heap (reclaims 60KB static array, shares dynamically).
    3.  **LTO:** Enable `CONFIG_LTO=y` to strip unused static data.
    4.  **Stack profiling:** Use `CONFIG_THREAD_ANALYZER=y` to shrink oversized thread stacks.
    5.  **Content length:** If 4096 breaks mqtt.tinygs.com, try 8192 before 16384. Each doubling costs ~4KB.
    6.  **Measure:** `CONFIG_SYS_HEAP_RUNTIME_STATS=y` is enabled — log heap high-water mark after TLS handshake.

### 6.5 RadioLib on Zephyr — LOW RISK
*   **The Problem:** Porting RadioLib (optimized for Arduino's blocking API) to Zephyr.
*   **The Risk:** Naive `k_sleep()` polling for LoRa packet reception negates the nRF52's power savings.
*   **Current Status:** SX1262 init fails with code -2 (`CHIP_NOT_FOUND`) — likely SPI pin mapping or Vext timing issue. Not blocking MQTT-TLS testing.
*   **Mitigations:**
    1.  Use Zephyr's `k_work` workqueue attached to `gpio_add_callback` for DIO1 (packet received interrupt).
    2.  Debug SPI communication: verify pin mapping against T114 schematic, increase Vext stabilization delay, check CS polarity.

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

### 6.9 Extreme Brittleness to Stack Overflows — HIGH RISK
*   **The Problem:** With static RAM at 241KB, only ~15KB of genuinely free RAM remains for the Zephyr kernel's dynamic thread stacks, ISR stacks, and network buffer allocations.
*   **The Risk:** OpenThread and mbedTLS state machines are complex. A malformed packet or edge-case TLS fragment could cause a function to allocate slightly more stack frames than profiled, triggering a CPU hard fault and rebooting the node.
*   **Mitigations:** During Phase 1, perform extensive "fuzzing". Send max-sized MQTT payloads, invalid Thread packets, and force TLS reconnections repeatedly while monitoring `CONFIG_THREAD_ANALYZER` high-water marks. Do not trust idle-state RAM usage.

### 6.10 Hardware Crypto (CC310) vs. Adafruit SoftDevice — HIGH RISK
*   **The Problem:** To save RAM, `CONFIG_PSA_CRYPTO_DRIVER_CC3XX=y` uses the nRF52's hardware crypto. However, the board retains the Adafruit UF2 bootloader and the Nordic SoftDevice at `0x00000 - 0x26000`.
*   **The Risk:** The SoftDevice (MBR) intercepts hardware interrupts. A Zephyr OpenThread application directly controlling the CC310 peripheral alongside a resident Nordic SoftDevice can lead to IRQ routing failures or crypto initialization crashes.
*   **Mitigations:** Verify early in Phase 1 that the CC310 initializes and successfully performs an ECC/RSA operation. If it faults, either drop the Adafruit bootloader entirely (migrating to MCUboot at `0x0` via SWD) or fall back to software crypto (which will break the 256KB RAM budget).