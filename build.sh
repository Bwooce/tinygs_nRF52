#!/bin/bash
# TinyGS Zephyr Build Script — NCS v3.3.0 / Zephyr 3.7 / SDK 0.17.4
#
# Optimised for 16 cores. Output in build/.

set -e

WORKSPACE_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
SDK_DIR="${ZEPHYR_SDK_DIR:-$HOME/zephyr-sdk-0.17.4}"
NCS_DIR="${WORKSPACE_DIR}/ncs_new"

source "${WORKSPACE_DIR}/.venv/bin/activate"
source "${NCS_DIR}/zephyr/zephyr-env.sh"

export ZEPHYR_SDK_INSTALL_DIR="${SDK_DIR}"

echo "Building Zephyr App against NCS v3.3.0 (SDK ${SDK_DIR##*-})..."
# Out-of-tree board lives at boards/heltec/heltec_mesh_node_t114/. Point
# west at it via BOARD_ROOT so HWMv2 board discovery picks it up.
CMAKE_BUILD_PARALLEL_LEVEL=16 west build \
    -b heltec_mesh_node_t114/nrf52840 \
    -d build \
    -- -DBOARD_ROOT="${WORKSPACE_DIR}" "$@"

UF2_FILE="${WORKSPACE_DIR}/build/tinygs_nRF52/zephyr/zephyr.uf2"
if [ -f "$UF2_FILE" ]; then
    UF2_SIZE=$(stat -c%s "$UF2_FILE")
    if [ "$UF2_SIZE" -gt 1300000 ]; then
        echo "WARNING: UF2 too large (${UF2_SIZE} > 1300000)! flash.sh will refuse to flash."
    elif [ "$UF2_SIZE" -gt 1200000 ]; then
        echo "WARNING: UF2 approaching limit (${UF2_SIZE}/1300000 bytes)"
    fi
fi

echo "Build Completed (build/)"
