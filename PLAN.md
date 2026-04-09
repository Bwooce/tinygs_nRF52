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
*   **Configuration:** WebBLE (Web Bluetooth) for browser-based, app-free setup.
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

### 3.3 Configuration Interface: WebBLE
*   We will expose a BLE peripheral service on the nRF52840.
*   Users can navigate to a static webpage (e.g., `config.tinygs.com/nrf52`) using Chrome or Edge.
*   Using the Web Bluetooth API (WebBLE), the browser can connect directly to the board to provision Thread Network Credentials, TinyGS Station ID, and Passwords. **No smartphone app required.**

### 3.4 RadioLib Zephyr Integration
RadioLib is natively designed for the Arduino ecosystem. To run it on Zephyr RTOS, we will create a custom HAL (Hardware Abstraction Layer) class inheriting from `RadioLibHal`.
*   **SPI & GPIO:** Map RadioLib's pin logic to Zephyr's Devicetree (`gpio_dt_spec`) and `spi_transceive` APIs.
*   **Interrupts:** Zephyr's `gpio_add_callback` will be used to trigger RadioLib's ISRs for LoRa packet reception via the SX1262 DIO1 pin.

## 4. Phased Implementation Plan

### Phase 1: High-Risk Prototyping (TLS Memory & Network)
Before porting the state machine, we must prove the 256KB RAM budget holds up during a full TLS handshake over Thread.
1.  **Zephyr Thread NAT64 Setup:** Configure Zephyr as an OpenThread MTD (Minimal Thread Device) and ensure it can synthesize NAT64 addresses to ping IPv4 internet servers.
2.  **mbedTLS + MQTT PoC:** Establish a persistent, native MQTT-TLS connection directly to `mqtt.tinygs.com`.
3.  **Memory Profiling:** Use Zephyr `ram_report` and Thread Analyzer to verify the heap and stack margins stay within the 222KB budget during the handshake and steady-state keep-alive phases. Ensure no memory leaks occur during disconnections/reconnections.
4.  **RadioLib Zephyr HAL PoC:** Implement the custom `ZephyrHal` class, integrate RadioLib, and verify SX1262 SPI communication.

### Phase 2: Core TinyGS Porting
1.  **State Machine & Polling:** Port the TinyGS state machine to Zephyr threads/workqueues. 
2.  **Protocol Emulation:** Ensure the JSON payloads and MQTT topic structures match the ESP32 version exactly.
3.  **Power Management:** Implement Zephyr deep sleep (System OFF or deep System ON) during idle periods. Rely on Thread SED polling for TCP keep-alives. Wake on SX1262 DIO1 interrupt (packet received).
4.  **Solar & Battery:** Read battery voltage via the T114 ADC and manage sleep cycles based on charge levels.

### Phase 3: Configuration & Peripherals
1.  **Configuration:** Implement the BLE GATT service for WebBLE provisioning.
2.  **Display (Optional):** Implement low-power updating of the TFT/OLED (only turning on upon user button press via GPIO interrupt).

## 5. Limitations & Requirements for Users
*   **Infrastructure:** Requires the user to have a standard Thread Border Router (e.g., Apple TV 4K, HomePod Mini, or Home Assistant SkyConnect) on their network.