#!/bin/bash
# TinyGS UF2 Flasher Script for Heltec T114 — NCS v3.3.0 sysbuild layout.
#
# Hard pre-flash safety check: parses UF2 to verify every block stays
# within the app partition (0x26000–0xE2000), refuses to flash if any
# block reaches into FATFS or the bootloader. The Adafruit bootloader
# silently rejects out-of-range blocks, so a "successful" UF2 copy can
# still half-brick the device — this catches it before we copy.

set -e

REPO_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
UF2_FILE="${REPO_DIR}/build/tinygs_nRF52/zephyr/zephyr.uf2"
SERIAL_LOG="${REPO_DIR}/serial_longrun.log"
SERIAL_SCRIPT="${REPO_DIR}/scripts/serial_log.py"

if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 firmware not found at $UF2_FILE"
    echo "Run ./build.sh first."
    exit 1
fi

UF2_SIZE=$(stat -c%s "$UF2_FILE")
UF2_MAX=1300000
if [ "$UF2_SIZE" -gt "$UF2_MAX" ]; then
    echo "ERROR: UF2 file too large! ${UF2_SIZE} bytes > ${UF2_MAX} limit."
    exit 1
fi
echo "UF2 size: ${UF2_SIZE} bytes (limit: ${UF2_MAX})"

# Bootloader-safety check — verify every UF2 block stays within app partition.
# Adafruit bootloader silently drops out-of-range blocks; without this check
# a partial flash could leave the device in an inconsistent state.
python3 - <<'PYEOF' "$UF2_FILE"
import struct, sys
APP_START = 0x00026000
APP_END   = 0x000E2000   # FATFS starts here
BL_START  = 0x000F4000   # bootloader — NEVER touch
with open(sys.argv[1], 'rb') as f:
    data = f.read()
addrs = []
for i in range(len(data) // 512):
    blk = data[i*512:(i+1)*512]
    magic1, magic2, flags, addr, payload = struct.unpack_from('<IIIII', blk, 0)
    if magic1 != 0x0A324655:
        continue
    addrs.append((addr, payload))
if not addrs:
    print("ERROR: UF2 has no valid blocks"); sys.exit(1)
lo = min(a for a, _ in addrs)
hi = max(a + p for a, p in addrs)
print(f"UF2 address range: 0x{lo:08X} .. 0x{hi:08X}")
print(f"Safe app range:    0x{APP_START:08X} .. 0x{APP_END:08X}")
print(f"Bootloader (RO):   0x{BL_START:08X}+")
if hi > BL_START:
    print("CRITICAL: UF2 OVERLAPS BOOTLOADER REGION — REFUSING TO FLASH"); sys.exit(2)
if hi > APP_END:
    print("ERROR: UF2 reaches into FATFS partition"); sys.exit(2)
if lo < APP_START:
    print("ERROR: UF2 starts before app partition (would touch MBR/SoftDevice)"); sys.exit(2)
print("SAFE: UF2 stays within app partition.")
PYEOF
SAFETY_RC=$?
if [ $SAFETY_RC -ne 0 ]; then
    echo "Refusing to flash — UF2 would write outside the app partition."
    exit 1
fi

# Stop serial logger
LOGGER_PID=$(pgrep -f "serial_log.py" 2>/dev/null || true)
if [ -n "$LOGGER_PID" ]; then
    kill $LOGGER_PID 2>/dev/null || true
    sleep 0.5
fi

# Identify the T114 specifically by USB VID — app firmware is 2fe3:0001
# (TinyGS Configurator), bootloader is 239a:0071 (HT-n5262). Anything else
# on the bus (e.g. a LilyGO T-Beam on /dev/ttyACM0) gets skipped.
TTY_PORT=""
for dev in /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$dev" ] || continue
    vid=$(udevadm info -q property "$dev" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
    if [ "$vid" = "2fe3" ] || [ "$vid" = "239a" ]; then
        TTY_PORT="$dev"
        break
    fi
done
if [ -n "$TTY_PORT" ]; then
    echo "Triggering 1200-baud reset on $TTY_PORT..."
    stty -F "$TTY_PORT" 1200 2>/dev/null || true
else
    echo "No T114 serial port found (looked for VID 2fe3 / 239a). Assuming device is already in bootloader mode."
fi

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
echo "Copying firmware..."
cp "$UF2_FILE" "$MOUNT_POINT/"
sync

echo "Flashing complete! Device will reboot."

if [ -f "$SERIAL_SCRIPT" ]; then
    sleep 2
    # Re-find the T114's port — it just rebooted into the new firmware
    # and may have re-enumerated to a different /dev/ttyACMn (especially
    # if other USB-serial devices like a T-Beam are attached).
    LOG_PORT=""
    for dev in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$dev" ] || continue
        vid=$(udevadm info -q property "$dev" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
        if [ "$vid" = "2fe3" ]; then
            LOG_PORT="$dev"
            break
        fi
    done
    LOG_PORT="${LOG_PORT:-/dev/ttyACM0}"
    nohup python3 "$SERIAL_SCRIPT" "$LOG_PORT" 115200 "$SERIAL_LOG" > /dev/null 2>&1 &
    echo "Serial logger restarted (PID $!) on $LOG_PORT"
fi
