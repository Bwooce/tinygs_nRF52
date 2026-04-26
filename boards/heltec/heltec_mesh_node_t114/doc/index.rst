.. _heltec_mesh_node_t114:

Heltec Mesh Node T114
#####################

Overview
********

The Heltec Mesh Node T114 (V2, model HT-n5262) is a compact LoRa node based
on the Nordic Semiconductor nRF52840 paired with a Semtech SX1262 sub-GHz
transceiver. The optional 1.14" 240x135 ST7789V TFT and L76K GNSS variant
are supported by this board definition.

References:

- Product page: https://heltec.org/project/mesh-node-t114/
- Schematic V2.1:
  https://resource.heltec.cn/download/Mesh_Node_T114/schematic/MeshNode-T114_V2.1.pdf
- Arduino BSP: https://github.com/HelTecAutomation/Heltec_nRF52
- Meshtastic variant.h: ``variants/nrf52840/heltec_mesh_node_t114/variant.h``

Hardware
********

- nRF52840 (ARM Cortex-M4F, 64 MHz, 1 MB flash, 256 KB SRAM)
- SX1262 LoRa transceiver, 150-960 MHz, DIO2 RF switch, DIO3 TCXO 1.8 V
- ST7789V 1.14" 240x135 RGB565 TFT (optional)
- L76K GNSS on UART1 (optional)
- USB Type-C (CDC ACM + UF2 mass storage bootloader)
- LiPo charger, 100k/390k divider on AIN2 (P0.04) for battery sense
- 1x green LED (P1.03) plus 2x SK6812 NeoPixel chain (P0.14, gated by
  Vext on P0.21)
- BOOT button on P1.10

Pin Assignments
***************

=================  ============  ====================================
Function           Pin           Notes
=================  ============  ====================================
LoRa SCK / MOSI    P0.19 / 22    SPI1
LoRa MISO / CS     P0.23 / 24    SPI1
LoRa BUSY          P0.17
LoRa DIO1          P0.20
LoRa RESET         P0.25
TFT SCK / MOSI     P1.08 / 09    SPI0
TFT CS / DC        P0.11 / 12
TFT RESET          P0.02
TFT Backlight      P0.15         active LOW
TFT Power Enable   P0.03         active LOW (VTFT_CTRL)
NeoPixel data      P0.14         SPI2 MOSI, dummy SCK on P0.09
Vext               P0.21         active HIGH, gates NeoPixels and GPS
ADC bias enable    P0.06         active HIGH, drives the divider
Battery sense      P0.04         AIN2, multiplier 4.916
BOOT button        P1.10         active LOW
GPS UART1 TX       P1.07
GPS UART1 RX       P1.05
=================  ============  ====================================

Flash Layout
************

The T114 ships with the Adafruit nRF52 UF2 bootloader at ``0xF4000``.
The application is positioned at ``0x26000`` (above the SoftDevice
region preserved by the bootloader). These addresses are baked into
every shipped Adafruit bootloader and **must not** be changed without a
coordinated bootloader re-flash via SWD.

==========  ========  ===================
Region      Size      Purpose
==========  ========  ===================
0x00000     152 KB    SoftDevice S140 (read-only)
0x26000     752 KB    Application
0xE2000      64 KB    FATFS storage (USB MSC)
0xF2000       8 KB    NVS settings
0xF4000      48 KB    Adafruit bootloader (read-only)
==========  ========  ===================

Programming and Debugging
*************************

Flashing is done by copying the generated UF2 image to the bootloader's
USB mass storage drive. The repository's ``flash.sh`` script automates
this: it triggers the 1200-baud reset, waits for the bootloader drive
to mount, copies the firmware, and restarts the serial monitor.

For SWD recovery (e.g. after a corrupted flash), pyocd or
openocd-nrf5 are configured by ``board.cmake``. nrfjprog and JLink
runners are *not* enabled because their default behaviour writes to
the SoftDevice region and would brick the UF2 bootloader.

The nRF52840 ships with a 32 kHz crystal (``USE_LFXO`` in the
Meshtastic variant), so ``CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL`` is
selected in the board's ``Kconfig.defconfig``.
