#!/bin/bash
# TinyGS Zephyr Build Script
# Forces Ninja to use 10+ cores to compile faster

set -e

WORKSPACE_DIR="/home/bruce/dev/tinygs_nRF52"

# Activate the virtual environment that has west installed
source "${WORKSPACE_DIR}/.venv/bin/activate"
source "${WORKSPACE_DIR}/ncs/zephyr/zephyr-env.sh"

echo "Building Zephyr App using West with at least 10 cores..."
# Ninja automatically parallelizes to all cores. We can strictly enforce it with an env var.
CMAKE_BUILD_PARALLEL_LEVEL=16 west build -b nrf52840dk_nrf52840 -d build

# Check UF2 size — warn if approaching the bootloader drive limit
UF2_FILE="${WORKSPACE_DIR}/build/zephyr/zephyr.uf2"
if [ -f "$UF2_FILE" ]; then
    UF2_SIZE=$(stat -c%s "$UF2_FILE")
    UF2_MAX=1000000
    if [ "$UF2_SIZE" -gt "$UF2_MAX" ]; then
        echo "WARNING: UF2 too large (${UF2_SIZE} > ${UF2_MAX})! flash.sh will refuse to flash."
    elif [ "$UF2_SIZE" -gt 900000 ]; then
        echo "WARNING: UF2 approaching limit (${UF2_SIZE}/1000000 bytes)"
    fi
fi

echo "Build Completed!"
