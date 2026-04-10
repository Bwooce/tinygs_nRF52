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

1.  **USB Composite Device (MSC + CDC ACM):** When plugged into a PC or Mac via USB-C, the device registers as a Composite USB Device. It presents both a standard USB Flash Drive (mounting the 24KB FATFS partition at 0xEC000) and a Virtual COM Port (CDC ACM) simultaneously. All Zephyr `LOG_INF()` and `printk()` output is automatically routed to this COM port so the user can monitor the Thread connection and MQTT handshakes in real-time using a serial monitor (like PuTTY or `screen`).
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
| Application | 0x26000 | 0xEC000 | 792KB | code_partition |
| FATFS Storage | 0xEC000 | 0xF4000 | 32KB | tinygs_storage |
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
    *   FATFS: 0xEC000 (32KB)
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

### Phase 1: High-Risk Prototyping (TLS Memory & Network)
Before porting the state machine, we must prove the 256KB RAM budget holds up during a full TLS handshake over Thread.
1.  **[IN PROGRESS] Zephyr Thread NAT64 Setup:** Configure Zephyr as an OpenThread MTD (Minimal Thread Device) and ensure it can synthesize NAT64 addresses to ping IPv4 internet servers. (Initial scaffolding complete).
2.  **mbedTLS + MQTT PoC:** Establish a persistent, native MQTT-TLS connection directly to `mqtt.tinygs.com`.
3.  **Memory Profiling:** Use Zephyr `ram_report` and Thread Analyzer to verify the heap and stack margins stay within the 222KB budget during the handshake and steady-state keep-alive phases. Ensure no memory leaks occur during disconnections/reconnections.
4.  **RadioLib Zephyr HAL PoC:** Implement the custom `ZephyrHal` class, integrate RadioLib, and verify SX1262 SPI communication.

### Phase 2: Core TinyGS Porting
1.  **State Machine & Polling:** Port the TinyGS state machine to Zephyr threads/workqueues. 
2.  **Protocol Emulation:** Ensure the JSON payloads and MQTT topic structures match the ESP32 version exactly.
3.  **Power Management:** Implement Zephyr deep sleep (System OFF or deep System ON) during idle periods. Rely on Thread SED polling for TCP keep-alives. Wake on SX1262 DIO1 interrupt (packet received).
4.  **Solar & Battery:** Read battery voltage via the T114 ADC and manage sleep cycles based on charge levels.

### Phase 3: Peripherals & Polish
1.  **Display (Optional):** Implement low-power updating of the TFT/OLED (only turning on upon user button press via GPIO interrupt).
2.  **Firmware Updates:** USB-only via UF2 drag-and-drop. OTA over Thread was evaluated and dropped due to power cost (see Section 6.3).

## 5. Limitations & Requirements for Users
*   **Infrastructure:** Requires the user to have a standard Thread Border Router (e.g., Apple TV 4K, HomePod Mini, or Home Assistant SkyConnect) on their network.

## 6. Architectural Risks & Constraints

### 6.1 Persistent TLS over Thread SED — HIGH RISK
*   **The Problem:** Maintaining a persistent TCP/TLS connection for MQTT over a Thread Sleepy End Device (SED).
*   **The Risk:** The Border Router's NAT64 state tables timeout after 120-300s. MQTT brokers behind load balancers send TCP RST after 60-90s idle. The device must send MQTT PINGREQ faster than both timeouts, forcing the 802.15.4 radio to wake every 30-60s — destroying the 11µA sleep target.
*   **Mitigations:**
    1.  **Measure during Phase 1:** Log actual NAT64 timeout and broker idle timeout by varying MQTT keep-alive intervals.
    2.  **Connect/disconnect pattern:** Connect → publish telemetry → subscribe → wait 30s for commands → disconnect → deep sleep for N minutes. Trades latency for power. TinyGS satellite passes are predictable — wake near pass windows only.
    3.  **TLS session resumption:** Enable `CONFIG_MBEDTLS_SSL_SESSION_TICKETS` to cache session state. Reconnection avoids the expensive full handshake (~50KB heap spike).
    4.  **Investigate MQTT 5.0 session expiry** on mqtt.tinygs.com — lets the broker hold subscriptions across disconnects.

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
*   **Resolution:** Split into two partitions: FATFS 24KB (0xEC000-0xF2000), NVS 8KB (0xF2000-0xF4000). Implemented in app.overlay. FATFS is also unmounted before `usb_enable()` to prevent concurrent access corruption.

### 6.7 Flash Wearout & Filesystem Durability — HIGH RISK
*   **The Problem:** FATFS in the 24KB partition does not perform hardware wear-leveling (unlike SD cards). The nRF52840 flash is rated for ~10,000 erase cycles.
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