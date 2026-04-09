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
- **Flash:** Use `./flash.sh`. It performs a 1200-baud auto-reset to trigger the UF2 bootloader and copies the firmware to the mounted drive.
- **Debugging:** All logs are routed to the USB CDC ACM serial port. Always use `LOG_INF`, `LOG_ERR`, etc.

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
- Support is provided via **MCUboot**. 
- Flash is split into two slots (~470KB each). 
- Firmware must be signed/formatted as a `.bin` for MCUboot, but `.uf2` is used for local USB flashing.
