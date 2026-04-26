# SPDX-License-Identifier: Apache-2.0
#
# The Heltec Mesh Node T114 ships with the Adafruit nRF52 UF2 bootloader.
# Flashing is done by copying build/<app>/zephyr/zephyr.uf2 to the
# bootloader's USB mass storage drive (via ./flash.sh in this repo).
#
# No west-flash runner is registered: the in-tree nrfjprog/JLink runners
# would write to the SoftDevice region (0x0..0x26000) and brick the UF2
# bootloader. Use ./flash.sh instead.
#
# SWD-attached debug/recovery is still possible via pyocd/openocd-nrf5,
# both of which respect FLASH_LOAD_OFFSET and only touch the application
# region — they are intentionally enabled here for recovery scenarios.

board_runner_args(pyocd "--target=nrf52840" "--frequency=4000000")
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-nrf5.board.cmake)
