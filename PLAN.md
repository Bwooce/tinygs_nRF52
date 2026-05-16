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

**Border Router Capability Matrix (For TinyGS Traffic):**
Using OpenThread's built-in local DNS64 synthesis (querying 1.1.1.1 via the Border Router's NAT64 prefix), here is how the device's traffic routes across major ecosystems today:

| Feature / Traffic Type | Home Assistant OTBR | Apple (HomePod / Apple TV) | Google (Nest Hub / Max) | Notes & Future Outlook |
| :--- | :--- | :--- | :--- | :--- |
| **Native Thread Joining** | ✅ Supported | ❌ Fails (Matter Only) | ❌ Fails (Matter Only) | Home Assistant is the only platform that exposes the underlying OpenThread Commissioner. It allows you to run `ot-ctl commissioner joiner add '*' TNYGS2026NRF`, which opens the mesh to any device that knows the password, completely bypassing Matter. This means Home Assistant is required at least to join the network. |
| **Local IPv6 Mesh Routing** | ✅ Supported | ✅ Supported | ✅ Supported | All BRs route local packets perfectly. |
| **DNS64 (Translation)** | ✅ Supported | ❌ Not Supported | ❌ Not Supported | We bypass this using local OpenThread synthesis and Cloudflare (1.1.1.1). |
| **NAT64 (Transit)** | ✅ Supported | ✅ Supported | ❌ Not Supported* | **This is the IPv4 internet bridge.** Google historically drops this traffic. |
| **DNS Queries** (UDP 53) | ✅ Transits | ✅ Transits | ❌ Fails | Requires NAT64. |
| **MQTT** (TCP 8883) | ✅ Transits | ✅ Transits | ❌ Fails | Requires NAT64. |
| **SNTP** (UDP 123) | ✅ Transits | ✅ Transits | ❌ Fails | Requires NAT64 (because we resolve `pool.ntp.org` via DNS). |
| **Multicast UDP** (`ff05::`) | ✅ Transits | ✅ Transits (If Checksum OK) | ✅ Transits (If Checksum OK) | Apple and Google strictly enforce MLDv2. (Requires PC Wi-Fi fix). |
| **mDNS / Bonjour** | ✅ Supported | ✅ Supported | ❌ Limited | Used for local discovery. |
| ***Future Update*:** | **N/A** (Already unlocked) | **Updating to Thread 1.4** | **Updating to Thread 1.4** | Thread 1.4 mandates NAT64. Once Google's early 2026 rollout completes, they should upgrade from ❌ to ✅ for transit. |

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

#### Future: MCUboot OTA with External SPI Flash (Phase 3+)
To achieve safe, power-fail-resilient FOTA updates without bricking the device, the bootloader must be transitioned to MCUboot using an **External SPI Flash** chip for the secondary slot. The internal 1MB flash is too small for a dual-bank layout once the application, SoftDevice, and FATFS drives are accounted for.

This requires a hardware modification and a **one-time, irreversible software migration** using SWD hardware.

**Hardware Prerequisites:**
1.  An SWD debug probe (J-Link EDU Mini ~$20, or any CMSIS-DAP probe).
2.  SWD access to the T114 board (test pads or header).
3.  A 4MB to 16MB external SPI flash breakout board (e.g., Winbond W25Q32/W25Q128 or Macronix MX25R32).
    *   *Warning:* Do **not** populate the empty MX25R16 footprint on the back of the T114 board. Heltec wired those pads in parallel with the SX1262 LoRa radio's SCK/MOSI/MISO, DIO1, and BUSY pins. Using this footprint will cause severe bus contention and crash the radio during flash writes.
    *   *Solution:* Use the **8-pin 1.25mm GPS connector**. This offboard connector provides 3V3 (Vext-gated), GND, and four GPS-module GPIOs that are completely unused in TinyGS. Use an 8-pin 1.25mm pigtail cable to wire the SPI flash breakout into this port. Map these pins to the completely unused `SPIM3` peripheral in Zephyr.

**GPS-port pin map (verified against Heltec schematic V2.1 + Meshtastic `variant.h`):**

| GPS port pin | nRF52840 pin | Original GPS function | Repurposed for SPI flash |
|---|---|---|---|
| 3V3 | (Vext rail, gated by P0.21) | GPS_EN — module power | Flash VCC; raise P0.21 HIGH for flash access, LOW when idle |
| GND | — | GND | Flash GND |
| GPS_TX | P1.07 | UART1 RX (CPU-side) | **SPIM3 MISO** |
| GPS_RX | P1.05 | UART1 TX (CPU-side) | **SPIM3 MOSI** |
| GPS_PPS | P1.04 | PPS input | **SPIM3 SCK** |
| GPS_STANDBY | P1.02 | Wake/sleep control | **SPIM3 CS** |
| GPS_RESET | P1.06 | Reset (datasheet ≥100 ms LOW) | unused (or secondary CS for a future second device) |
| (8th pin) | — | varies by harness | leave unconnected |

All four signal pins are on the P1 GPIO port and are otherwise idle in the TinyGS firmware (the GPS module is not used; UART1 is unbound in our overlay), so no pinmux conflicts. SPIM3 supports any pinctrl mapping on the nRF52840 — there is no peripheral-pin restriction here.

*SPIM3 caveat:* SPIM3 is the high-speed peripheral and draws ~1 mA more than SPIM0/1/2 when active. PM_DEVICE_RUNTIME + `SPI_NOR_ACTIVE_DWELL_MS` gates the chip idle (the chip itself enters DPD, ~1 µA, after the dwell timeout); active use during OTA streaming is short, so the SPIM3 extra current is paid only during updates.

**Actual board (2026-05-07): Winbond W25Q128JVSQ on a 6-pin breakout** (VCC/CS/DO/GND/CLK/DI exposed; /WP and /HOLD pulled high on-board through R1=2 kΩ array; LED draws ~1.5 mA continuously from Vext — desolder if soak-power-critical). 16 MB capacity, JEDEC ID `ef 40 18`, 104 MHz max SPI. Wired per the table above.

**Migration Steps:**
1.  **[DONE 2026-05-07]** Wire the external SPI flash to the GPS port pigtail.
2.  **[DONE 2026-05-07]** Update `app.overlay` to define `spi3` (pinctrl: P1.04/05/07, CS on P1.02) and declare `ext_flash: w25q128@0` with `compatible = "jedec,spi-nor"`, JEDEC ID `[ef 40 18]`, 16 MB size, DPD timings. Boot probe (`ext_flash_probe()` in main.cpp) is fail-safe: if the breakout isn't wired, the driver fails JEDEC verification, `device_is_ready()` returns false, and the app logs the absence and continues. `/status` exposes `ext_flash: 0/1`.
3.  Connect SWD probe to the T114.
4.  Erase the full internal flash: `nrfjprog --eraseall` (destroys Adafruit UF2 bootloader and SoftDevice).
5.  Update `prj.conf`: set `CONFIG_BOOTLOADER_MCUBOOT=y`.
6.  Remap partitions to MCUboot + golden + log-volume layout (16 MB external = plenty of room for a permanent factory-recovery image alongside Slot 1):

    | Partition | Where | Size | Purpose |
    |---|---|---|---|
    | MCUboot | internal @ 0x00000 | ~32 KB | bootloader |
    | Slot 0 (active app) | internal @ 0x08000 | ~800 KB | runtime image |
    | FATFS (USB config drive) | internal | 64 KB | config.json over USB MSC |
    | NVS settings | internal | 8 KB | lat/lon/MQTT/TZ/etc. |
    | Slot 1 (OTA staging) | external @ 0x000000 | ~800 KB | OTA upload target |
    | **Golden image** | external @ 0x100000 | ~800 KB | factory-known-good, never overwritten by OTA |
    | LittleFS log volume | external @ 0x200000 | ~14 MB | logs, capture rings, debug snapshots |

    LittleFS has native dynamic wear-leveling; with W25Q128JV's 100k erase cycles per 4 KB sector × ~3500 free sectors, total useful sector erases ≈ 3.5 × 10⁸ — the chip will outlive anything we throw at it. Configure via `CONFIG_FILE_SYSTEM_LITTLEFS=y` + a `zephyr,fstab` DTS node mounted on the log partition.

7.  Ensure power management is on so the SPI flash hits 1 µA DPD when idle. Without this, the chip idles at 10–50 µA:
    ```kconfig
    CONFIG_PM_DEVICE=y
    CONFIG_PM_DEVICE_RUNTIME=y
    CONFIG_FLASH=y
    CONFIG_SPI_NOR=y
    CONFIG_SPI_NOR_ACTIVE_DWELL_MS=100   # NCS v3.3+ replacement for the old SPI_NOR_IDLE_IN_DPD
    ```
8.  Flash MCUboot: `west flash --runner jlink` targeting the MCUboot child image.
9.  Flash signed application image via `west flash` or `mcumgr` over USB serial.

**Golden-image restore (implemented at application layer, no MCUboot patches):**

The application reserves an 800 KB "Golden" partition in external flash and never writes to it during OTA. Restore is triggered by one of:
- `/restore` POST on the web UI (Basic-auth gated, requires admin password)
- A user-button hold at boot (only viable once we add a button or repurpose RST timing)
- An NVS magic flag (`tgs/restore`) set via the config-form or by a one-shot MQTT command

On any of these triggers, the app:
1. Streams the Golden partition into Slot 1 (`flash_copy(golden, slot1, 800 KB)`)
2. Calls `boot_request_upgrade(BOOT_UPGRADE_TEST)` to set MCUboot's swap-pending flag
3. `sys_reboot(SYS_REBOOT_COLD)`

MCUboot then performs its normal Slot 0 ↔ Slot 1 swap on next boot, the app boots from Golden, and the user can re-OTA from there. Escape hatch (Slot 0 too broken to even run the restore): SWD reflash, same as today's UF2 brick recovery.

To populate Golden the first time: build the signed image as usual, then `west flash --runner jlink` it to the Golden partition address (not Slot 0) before deploying. Subsequent OTAs leave Golden untouched.

**Image signing & key management:**

*   **Algorithm:** ECDSA-P256 (MCUboot default; ~10 KB smaller than RSA-2048 in the bootloader and validates ~3× faster on Cortex-M4F). Configure `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y` in the MCUboot child image.
*   **Key model:** **single fleet key**. One ECDSA-P256 keypair signs every image for every device. Per-device keys are not justified at this fleet size and would block recovery flashing.
*   **Private key location:** `keys/tinygs-fota-priv.pem` in this repo, **gitignored** and stored offline (USB stick + paper-printed PEM in a safe). Never on a CI runner, never on the device.
*   **Public key compilation:** `imgtool keygen -k keys/tinygs-fota-priv.pem -t ecdsa-p256` then `imgtool getpub -k keys/tinygs-fota-priv.pem > bootloader/key.c` so MCUboot links the pubkey statically. Public key in clear is fine — verification only.
*   **Signing at build:** `imgtool sign --key keys/tinygs-fota-priv.pem --version <ver> --header-size 0x200 --slot-size 0x80000 zephyr.bin signed.bin`. Wire into `build.sh` as a final step; unsigned images don't ship.
*   **Key rotation:** requires a one-time SWD-flash of every device. Treat as a multi-year cadence; rotate only on suspected compromise. Migration path: MCUboot supports two pubkeys simultaneously during a transition window.
*   **Manifest schema** (served by tinygs.com or a user-supplied URL): `{"version": <int>, "url": "<https-url>", "size": <bytes>, "sha256": "<hex>", "signed_image": true}`. Device verifies SHA over streamed bytes before handing off to MCUboot; MCUboot re-verifies the embedded ECDSA signature on the next boot. Two layers — SHA catches transport corruption, signature catches authenticity.

**Confirm-on-boot semantics:**

MCUboot installs new images as `pending` — they swap once, then if `boot_set_confirmed()` isn't called within the next reboot cycle, the swap reverts to the previous image. Application is responsible for confirming a boot is healthy. Definition of "healthy enough to confirm":

1.  Thread role is `Child` (or higher).
2.  MQTT-TLS handshake completed and CONNACK received.
3.  At least one PINGRESP received after CONNACK (proves the connection survives traffic, not just initial handshake).
4.  All three above persist for ≥ 5 minutes of continuous uptime.

When all four hold, call `boot_write_img_confirmed()`. If any of (1)-(3) fails over an extended retry budget (suggested: 30 min and ≥ 5 reconnect attempts), do **not** confirm and trigger `sys_reboot(SYS_REBOOT_COLD)`. MCUboot's swap-on-reboot will then revert.

This rule blocks two classes of bad image:

*   *Boots but can't reach broker* (e.g. broken DNS code, bad cert): rolled back automatically.
*   *Boots but immediately panics on first MQTT publish*: the panic causes a reboot before 5 min of confirmed activity, also rolled back.

**Migration brick recovery:**

If the MCUboot install fails mid-flight (USB cable yanked, image corrupt, signing key wrong), the device is dead until SWD reflash. Recovery procedure:

1.  Reconnect SWD probe; verify `nrfjprog --readback` reports the chip.
2.  `nrfjprog --eraseall` to clear partial state.
3.  `west flash --runner jlink` to reflash MCUboot.
4.  `mcumgr image upload signed.bin && mcumgr image confirm` to load the application.

Keep one good `mcuboot.hex` + one good `signed-app.bin` archived in the repo (or in `keys/recovery/`) for every release so recovery doesn't depend on having a working build environment.

**Delivery path (HTTPS-client, not MQTT, not on-device server):**

*   Device polls a manifest URL (configurable; default `https://api.tinygs.com/v1/firmware/<board>/latest`) on a long cadence (default once/24 h, plus on every boot, plus on demand via the `/firmware` web UI button or MQTT `cmnd/update`).
*   If the manifest's version > running version, the device streams the signed image over HTTPS (reuses the existing mbedTLS stack — no new client code beyond a thin chunked GET) directly into Slot 1, verifying SHA-256 inline.
*   On SHA match, call `boot_set_pending()` and `sys_reboot(SYS_REBOOT_COLD)`. MCUboot ECDSA-verifies and swaps.
*   The HTTP **server** (§21) is not on the OTA delivery path — it only *triggers* the client. They share no transport code.

**After migration:**
*   **Safe FOTA:** Production OTA via the HTTPS-client delivery path above. The image is streamed into External Flash (Slot 1); MCUboot atomically swaps it into internal flash on reboot. The web UI's `/firmware` endpoint is a trigger only — actual bytes never traverse the on-device HTTP server.
*   **Automatic Rollback:** If the new firmware fails the confirm-on-boot rule above, MCUboot automatically reverts to the old firmware on the next reboot.
*   **Development Flashing:** USB drag-and-drop (.uf2) is gone. Local development flashing is now done via `mcumgr` over the USB serial port, which uses the exact same secure staging/swap architecture.

**SoftDevice consequence:** `nrfjprog --eraseall` destroys both the Adafruit UF2 bootloader **and** the SoftDevice (BLE stack at 0x0–0x26000). TinyGS does not use BLE so there is no functional loss, but this is a one-way door — there is no path back to the UF2 bootloader without a SoftDevice reflash. Document this in the release notes for any user attempting the migration.

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
2.  **[DONE] MQTT-TLS over Thread:** Native TLS handshake via local NAT64 synthesis. Authenticated to mqtt.tinygs.com:8883 with real credentials. TLS session caching enabled.
3.  **[DONE] SX1262 LoRa Radio:** RadioLib `begin()` succeeds over SPI. Pin mapping verified against T114 v2 schematic.
4.  **[DONE] RAM Budget:** FLASH 453KB/752KB (59%), RAM 162KB/256KB (62%). Comfortable. Dedicated 40KB mbedTLS heap + 8KB system heap. LTO saves ~85KB flash.
5.  **[DONE] USB MSC + CDC ACM:** FATFS at 0xE2000, NVS at 0xF2000. 1200-baud bootloader entry working.
6.  **[OBSERVING] MQTT Connection Durability:** Logging PINGRESP intervals and disconnect events to measure NAT64/broker timeouts.

**Key technical decisions from Phase 1:**
*   CC310 hardware crypto disabled — PSA RSA verify broken, Oberon software crypto used instead.
*   `CONFIG_MBEDTLS_USE_PSA_CRYPTO=n` — legacy RSA verify path required for ECDHE-RSA-CHACHA20-POLY1305-SHA256.
*   Local DNS64 synthesis via OpenThread `OT_DNS_NAT64_ALLOW` for hostname resolution — queries 1.1.1.1 and returns locally-synthesized NAT64-routable AAAA addresses.
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
    | `GET /` | Root page: dashboard/config/firmware/restart buttons + OTP code | **Port** — keep firmware button (FOTA unlocked by Phase 3) |
    | `GET /logo.png` | TinyGS logo | **Port** — same PNG from `logos.h`, ~2.5 KB |
    | `GET /config` | Station config form (lat/lon, MQTT, board, TZ, modemStartup, boardTemplate, adv_prm) | **Port** — POST handler writes NVS, triggers reboot on MQTT changes |
    | `GET /dashboard` | SVG world map + cards (GS/modem/sat/last packet) + web serial console | **Port** — same layout, feed from same `status` struct |
    | `GET /restart` | Confirm page + reboot | **Port** — `sys_reboot(SYS_REBOOT_COLD)` |
    | `GET /cs?c1=<cmd>&c2=<counter>` | Console poll + command (`!p` test packet, `!w` weblogin, `!e` reset, `!o` OTP) | **Port** — hook into our existing log ring buffer |
    | `GET /wm` | Worldmap data (CSV of sat pos + modem + GS status + sat data + last packet) | **Port** — exact same payload shape, fed from `status` |
    | `POST /firmware` | FOTA update upload | **Port** — streams upload to external SPI flash for MCUboot |

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

    **Build phasing (revised — `/cs` ahead of `/wm`):**
    1. **[DONE 2026-05-04]** HTTP skeleton. In-tree `CONFIG_HTTP_SERVER`, listening socket on `[::]:80`. Live: `/`, `/dashboard`, `/status`, `/restart`. Reachable as `http://tinygs-<station>.local/` via SRP-published mDNS, plus IPv6-literal fallback.
    2. **[DONE 2026-05-04]** `/cs` console short-poll + dedicated 4 KB log-backend ring. Wall-clock-prefixed lines (`HH:MM:SS `) once SNTP has synced; `??:??:?? ` placeholder before. Bypasses the broken Apple-BBR multicast path entirely.
    3. **[DONE 2026-05-04]** Dashboard HTML + `/wm` CSV poll. 459-rect SVG worldmap (gzipped → 4 KB on the wire), four status cards, animated red sat dot. `radio_mutex` (in `tinygs_protocol.cpp`) wraps the begine/batch_conf field-update block, doppler_update, and lora_check_rx writers; `/wm` snapshot reads under the same lock.
    4. **[DONE 2026-05-04]** `/config` GET form + POST → NVS. HTTP Basic auth (`admin / cfg_admin_pw`, default `tinygs`) gates `/config`, `/restart`, and any `/cs?c1=<cmd>` write. Read-only paths stay open. Auth-gated commands implemented: `!e` reboot, `!w` weblogin request, `!p` test TX (cfg_tx_enable-gated). `/favicon.ico` and `/logo.png` ripped from the ESP32 build for visual parity.
    5. **`/firmware` trigger** — *blocked on §3.5 hardware* (external SPI flash + MCUboot migration). POST will kick off the HTTPS-client manifest poll + image stream into Slot 1; UI will show progress from a status struct. No multipart upload — bytes never traverse the on-device server.
    6. (Optional) mTLS upgrade if threat model changes or the OTBR firewall is enabled. Not started; not blocked.

    **End-of-phase-4 footprint:** FLASH 579 KB / 752 KB (77 %), RAM 244 KB / 256 KB (95 %). RAM tight; the bumped `NET_BUF_*_COUNT` pools (32 → 64 each) and `OPENTHREAD_PKT_LIST_SIZE` 10 → 24 absorbed boot-time `net_pkt: Data buffer allocation failed` and TLS-handshake `Packet list is full` events. STATUS line gained `nbuf=rx<peak>/<total>,tx<peak>/<total>` and `pkt=rx,tx` so future pool sizing is data-driven. iot_log status counter renamed `drop` to aggregate `send + inactive + nofmt + zlen + rfull` rather than just send-layer failures.

    **Remaining ESP32 IoTWebConf gaps (all unblocked, all optional):**
    *   `/config` form is missing `boardTemplate`, `modemStartup`, `adv_prm` (ESP32 has them with a tabular dict-table JSON editor). The values are still settable via MQTT — just no form field. Add cost: ~3 KB raw / ~1 KB gzipped for the editor JS, plus form-field rows.
    *   Local timezone — **DONE**. Ported the ESP32 fork's 460-entry POSIX TZ table verbatim (`src/tinygs_tz.{h,cpp}`). NVS key `tz` stores the index; `tinygs_tz_apply()` runs `setenv("TZ",...)` + `tzset()` on boot and on `/config` save; `localtime_r()` formats web-UI and /cs timestamps. Cost: 42 KB flash (36 KB tables + 5.5 KB picolibc tzset/localtime_r), 124 B BSS. UI dropdown wiring (`/config` form field) still pending.
    *   First-run forced-password setup. Explicit divergence from ESP32 (Thread join already gates LAN trust); not coming.
    *   `HEAD /dashboard` returns 405. Zephyr static-resource handler is hard-coded GET-only at `subsys/net/lib/http/http_server_http1.c:144`. Browsers always GET, so this only affects `curl -I` / proxy probes. Upstream-patch territory.
    *   Concurrency wall: 4-thread × 24 KB simultaneous burst still saturates net_buf TX pool. Real browser usage doesn't trigger it; documented limit.

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

### Phase 2.5: NCS Upgrade v2.6.0 → v3.3.0 — **DONE 2026-04-26**
Migrated on the `ncs-v3.3-upgrade` branch. Key fixes that landed:
- **OpenThread API:** `ctx->instance` is no longer populated by the L2 layer
  in v3.3 — replaced 16+ call sites with `openthread_get_default_instance()`.
- **SPI CS:** Zephyr v3.4 added `cs.cs_is_gpio` to `spi_cs_control`. Fixed
  ZephyrHal to clear it (was hard-faulting in `_spi_context_cs_control` on
  every SPI op). Fix committed to the RadioLib `zephyr-hal` branch.
- **MIPI DBI:** ST7789V display node rewritten as `mipi_dbi_st7789v` wrapping
  `&spi0` (Zephyr 3.7 requirement).
- **Settings backend:** v3.0+ defaulted to ZMS; pinned `CONFIG_SETTINGS_NVS=y`
  to keep our partition layout.
- **mbedTLS X.509-over-PSA:** RSA-signed server certs need
  `MBEDTLS_LEGACY_CRYPTO_C=y` + `MBEDTLS_RSA_C=y` (Nordic's PSA path doesn't
  populate `oid.c`'s sig-alg descriptor table without it). And
  `MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED=y` (defaults n with OPENTHREAD).
- **Sysbuild:** `SB_CONFIG_PARTITION_MANAGER=n` (NCS Partition Manager
  replaced by sysbuild-managed PM).
- **Stack bump:** Main stack 8KB → 12KB; mbedTLS v3.x routes X.509/TLS
  through PSA driver wrappers (an extra layer of frames over v2.6's direct
  legacy mbedtls), and Oberon's PSA driver does ECDSA/ECDH curve operations
  on the stack for constant-time safety. v2.6 high-water sat at ~3.5KB; v3.3
  jumped to ~6KB measured during MQTT-TLS handshake.
- **Build/flash scripts:** `build_v33.sh`/`flash_v33.sh` renamed to
  `build.sh`/`flash.sh` after stable validation. Old v2.6 NCS workspace
  preserved at `./ncs/` for archival.

Verified: TLS handshake to mqtt.tinygs.com:8883 succeeds; SAT positions and
TLE updates flow end-to-end. Memory: 504KB flash (65%), 195KB RAM (75%).

**Open follow-up: USB-next stack migration & the SCSI-eject dead-end.**

Two separate questions tangled together; this section pulls them apart.

**Q1: Does Zephyr publish a SCSI-eject event we can hook?**

No — neither in the legacy stack nor in USB-next. Both implementations handle `START_STOP_UNIT` (0x1B) **internally**: `usbd_msc_scsi.c::SCSI_CMD_HANDLER(START_STOP_UNIT)` updates the `medium_loaded` flag in the class state but does not raise any event upward. The `usbd_msg` event enum has no `USBD_MSG_MSC_*` entries at all. So the originally-hoped-for "OS-eject triggers config-apply on the device" shortcut does not exist as a published event in either stack.

Two ways around this if we need eject-reboot:
1. **Upstream a new `USBD_MSG_MSC_EJECT` event** in the USB-next stack — one-line addition in the SCSI handler + new enum value. Paired with a migration to USB-next (see Q2). Carries upstream-PR cycle risk.
2. **Patch the legacy stack locally** to add a `START_STOP_UNIT` handler that calls a weak app callback. ~10 lines, but lives as a Zephyr-module overlay or local fork.

Current behaviour (acceptable as-is): *physical unplug* of the USB cable triggers the reboot via the existing VBUS-edge path in `usb_vbus_work_handler`. Only the "eject from OS without unplug" shortcut is missing. Soak and normal config-edit flow are unaffected.

**Q2: Should we migrate from the legacy `usb_*` to the USB-next `usbd_*` stack anyway?**

The legacy stack is **deprecated** in NCS v3.3 (build emits `Deprecated symbol USB_DEVICE_DRIVER is enabled`) but still functional. USB-next is the long-term API.

Migration scope: 2–3 days, touching CDC ACM init, USB MSC class registration, composite descriptor declaration, and the `usb_enable` → `usbd_init`+`usbd_enable` lifecycle. VBUS-detect via `NRF_POWER->USBREGSTATUS` continues to work. Reference: `ncs_new/zephyr/samples/subsys/usb/usbd/`.

Migration on its own does **not** solve the eject problem (see Q1) — both stacks have the same gap. So the migration should be triggered by *some other feature* wanting USB-next: WebUSB transport, USB-CDC-NCM (Ethernet-over-USB) for an alternative device-management path, or upstream activity that retires the legacy stack outright. Until then, treat the deprecation warning as a noisy log line; do not migrate to silence it.

**Decision (2026-05-04):** keep the legacy stack. SCSI-eject is parked. Re-evaluate when the first feature actually wanting USB-next lands, or when an NCS release bumps the deprecation to a build-failing error.

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
    - **300s keepalive: WORKS** — confirmed with 2+ consecutive PINGRESPs via NAT64
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

### 6.3 OTA Firmware Updates — DEFERRED to Phase 3+ (external SPI flash + MCUboot)
*   **Phase 1/2:** USB-only via UF2 drag-and-drop. The Adafruit UF2 bootloader has no dual-slot capability and the internal 1 MB flash is too tight for an in-place dual-bank layout once SoftDevice + FATFS are accounted for. No OTA on the deployed firmware.
*   **Phase 3+:** OTA enabled via the External SPI Flash + MCUboot migration described in §3.5. Delivery is **HTTPS-client over Thread/NAT64** (manifest poll + signed image stream); MQTT is used only for "trigger an immediate poll" notifications, not for the byte stream. The 802.15.4 throughput concern from the original Phase-1 decision still applies to MQTT-delivered firmware — that's why Phase 3 uses HTTPS GET, not MQTT publish.
*   **OTA-over-Thread payload size:** still 500–700 KB. With NAT64+TLS over a 250 kbps Thread link the realistic transfer time is several minutes of pinned RX, but this is a once-per-update event (not the steady-state) and trades nicely against the alternative of physical USB access for every release.
*   **Coexistence:** the in-place stream-and-overwrite approach considered earlier (use `__ramfunc` writer + VTOR relocation, no external flash) is **superseded** by §3.5. The external-flash plan trades hardware modification for substantially lower brick risk and standard MCUboot rollback.

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