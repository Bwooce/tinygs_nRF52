#!/bin/bash
# TinyGS Zephyr Build Script
# Forces Ninja to use 10+ cores to compile faster
#
# Usage: ./build.sh [--ram-report]
#   --ram-report  Show top RAM consumers (BSS + data symbols) after build

set -e

WORKSPACE_DIR="/home/bruce/dev/tinygs_nRF52"
SDK_DIR="/home/bruce/zephyr-sdk-0.16.8"
NM="${SDK_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm"
ELF="${WORKSPACE_DIR}/build/zephyr/zephyr.elf"

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

# RAM report — show largest BSS/data symbols
if [[ "$1" == "--ram-report" ]] && [ -f "$ELF" ] && [ -x "$NM" ]; then
    echo ""
    echo "=== RAM Report: Top 30 BSS+Data symbols ==="
    "$NM" --size-sort -r "$ELF" | grep -i " [bBdD] " | head -30
    echo ""
    echo "=== RAM Report: Summary by section ==="
    "$NM" --size-sort -r "$ELF" | grep -i " [bB] " | awk '{sum += strtonum("0x"$1)} END {printf "BSS (zero-init):  %d bytes\n", sum}'
    "$NM" --size-sort -r "$ELF" | grep -i " [dD] " | awk '{sum += strtonum("0x"$1)} END {printf "Data (init):      %d bytes\n", sum}'
fi

# Flash report — show top code and rodata consumers
if [[ "$1" == "--flash-report" ]] && [ -f "$ELF" ] && [ -x "$NM" ]; then
    echo ""
    echo "=== Flash Report: Top 30 code (.text) symbols ==="
    "$NM" --size-sort -r "$ELF" | grep -i " [tT] " | head -30
    echo ""
    echo "=== Flash Report: Top 30 const data (.rodata) symbols ==="
    "$NM" --size-sort -r "$ELF" | grep -i " [rR] " | head -30
    echo ""
    echo "=== Flash Report: Summary by section ==="
    "$NM" --size-sort -r "$ELF" | grep -i " [tT] " | awk '{sum += strtonum("0x"$1)} END {printf "Text (code):   %d bytes (%.0fKB)\n", sum, sum/1024}'
    "$NM" --size-sort -r "$ELF" | grep -i " [rR] " | awk '{sum += strtonum("0x"$1)} END {printf "Rodata (const): %d bytes (%.0fKB)\n", sum, sum/1024}'
fi
