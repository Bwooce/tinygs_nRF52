#!/bin/bash
# TinyGS UF2 Flasher Script for Heltec T114
# Stops serial logger, triggers 1200-baud reset, waits for bootloader,
# copies UF2, restarts serial logger.
set -e

# Script-relative — works no matter where the repo lives.
REPO_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
UF2_FILE="${REPO_DIR}/build/zephyr/zephyr.uf2"
SERIAL_LOG="${REPO_DIR}/serial_longrun.log"
SERIAL_SCRIPT="${REPO_DIR}/scripts/serial_log.py"

if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 firmware not found! Run ./build.sh first."
    exit 1
fi

# Guard: check UF2 size before attempting to flash.
UF2_SIZE=$(stat -c%s "$UF2_FILE")
UF2_MAX=1000000
if [ "$UF2_SIZE" -gt "$UF2_MAX" ]; then
    echo "ERROR: UF2 file too large! ${UF2_SIZE} bytes > ${UF2_MAX} limit."
    echo "The firmware won't fit on the UF2 bootloader drive."
    echo "Reduce firmware size (disable debug logging, CC310, etc.)"
    exit 1
fi
echo "UF2 size: ${UF2_SIZE} bytes (limit: ${UF2_MAX})"

# Step 0: Stop serial logger (holds the serial port)
LOGGER_PID=$(pgrep -f "serial_log.py" 2>/dev/null || true)
if [ -n "$LOGGER_PID" ]; then
    kill $LOGGER_PID 2>/dev/null || true
    sleep 0.5
fi

# Step 1: Find the serial port (application firmware)
TTY_PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1)

if [ -n "$TTY_PORT" ]; then
    echo "Triggering 1200-baud reset on $TTY_PORT..."
    stty -F "$TTY_PORT" 1200 2>/dev/null || true
else
    echo "No serial port found. Assuming device is already in bootloader mode."
fi

# Step 2: Wait for bootloader drive to mount
echo "Waiting for UF2 bootloader drive..."
MOUNT_POINT=""
for i in $(seq 1 15); do
    for mp in /media/$USER/HT-n5262 /media/HT-n5262 /run/media/$USER/HT-n5262; do
        if [ -d "$mp" ]; then
            MOUNT_POINT="$mp"
            break 2
        fi
    done
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

# Step 4: Restart serial logger in background
if [ -f "$SERIAL_SCRIPT" ]; then
    sleep 2  # Wait for USB re-enumeration
    nohup python3 "$SERIAL_SCRIPT" /dev/ttyACM0 115200 "$SERIAL_LOG" > /dev/null 2>&1 &
    echo "Serial logger restarted (PID $!)"
fi
