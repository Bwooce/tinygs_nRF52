#!/bin/bash
# TinyGS UF2 Flasher Script for Heltec T114
# Triggers 1200-baud reset, waits for bootloader, copies UF2.
set -e

UF2_FILE="/home/bruce/dev/tinygs_nRF52/build/zephyr/zephyr.uf2"

if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 firmware not found! Run ./build.sh first."
    exit 1
fi

# Guard: check UF2 size before attempting to flash.
# The Heltec T114 UF2 bootloader drive is ~1MB. Files larger than
# this overflow the drive causing I/O errors (no bricking, but no flash).
UF2_SIZE=$(stat -c%s "$UF2_FILE")
UF2_MAX=1000000
if [ "$UF2_SIZE" -gt "$UF2_MAX" ]; then
    echo "ERROR: UF2 file too large! ${UF2_SIZE} bytes > ${UF2_MAX} limit."
    echo "The firmware won't fit on the UF2 bootloader drive."
    echo "Reduce firmware size (disable debug logging, CC310, etc.)"
    exit 1
fi
echo "UF2 size: ${UF2_SIZE} bytes (limit: ${UF2_MAX})"

# Step 1: Find the serial port (application firmware)
TTY_PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1)

if [ -n "$TTY_PORT" ]; then
    echo "Triggering 1200-baud reset on $TTY_PORT..."
    stty -F "$TTY_PORT" 1200 2>/dev/null || true
else
    echo "No serial port found. Assuming device is already in bootloader mode."
fi

# Step 2: Wait for bootloader drive to mount
# The udev rule auto-mounts, but udisks may need a moment
echo "Waiting for UF2 bootloader drive..."
MOUNT_POINT=""
for i in $(seq 1 15); do
    # Check common mount points
    for mp in /media/bruce/HT-n5262 /media/HT-n5262 /media/$USER/HT-n5262; do
        if [ -d "$mp" ]; then
            MOUNT_POINT="$mp"
            break 2
        fi
    done
    # Try mounting if /dev/sda appeared but isn't mounted
    if [ -b /dev/sda ] && [ -z "$MOUNT_POINT" ]; then
        MOUNT_POINT=$(udisksctl mount -b /dev/sda 2>/dev/null | grep -oP 'at \K.*' || true)
        if [ -n "$MOUNT_POINT" ]; then
            break
        fi
    fi
    sleep 1
done

if [ -z "$MOUNT_POINT" ]; then
    echo "Error: Bootloader drive did not appear after 15s."
    echo "Try double-tapping the RST button on the board."
    exit 1
fi

echo "Drive found at $MOUNT_POINT"

# Step 3: Copy firmware
echo "Copying firmware..."
cp "$UF2_FILE" "$MOUNT_POINT/"
sync

echo "Flashing complete! Device will reboot."
