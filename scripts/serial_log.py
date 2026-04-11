#!/usr/bin/env python3
"""Serial monitor that logs to file and stdout.

Handles USB CDC ACM disconnects (e.g., during USB re-enumeration at boot)
by automatically reconnecting when the port reappears.
"""
import serial
import sys
import os
import time
from datetime import datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
LOGFILE = sys.argv[3] if len(sys.argv) > 3 else "serial.log"


def wait_for_port(port, timeout=30):
    """Wait for serial port to appear, return True if found."""
    for _ in range(timeout * 2):
        if os.path.exists(port):
            return True
        time.sleep(0.5)
    return False


def connect(port, baud):
    """Open serial port, return serial object or None."""
    try:
        return serial.Serial(port, baud, timeout=1)
    except (serial.SerialException, OSError):
        return None


def main():
    print(f"Serial logger: {PORT} @ {BAUD} -> {LOGFILE}")
    print("Reconnects automatically on USB disconnect.\n")

    with open(LOGFILE, "a") as f:
        while True:
            # Wait for port
            if not wait_for_port(PORT, timeout=60):
                print(f"ERROR: {PORT} not available after 60s")
                sys.exit(1)

            # Small delay for port to stabilize after USB enumeration
            time.sleep(0.3)

            ser = connect(PORT, BAUD)
            if ser is None:
                time.sleep(1)
                continue

            f.write(f"\n=== Session started {datetime.now().isoformat()} ===\n")
            f.flush()
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Connected to {PORT}")

            try:
                while True:
                    line = ser.readline()
                    if line:
                        text = line.decode("utf-8", errors="replace").rstrip()
                        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        out = f"[{ts}] {text}"
                        print(out)
                        f.write(out + "\n")
                        f.flush()
            except (serial.SerialException, OSError):
                # Port disconnected (USB re-enumeration, device reboot, etc.)
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Port disconnected, waiting to reconnect...")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(1)
                continue
            except KeyboardInterrupt:
                print("\n--- Stopped ---")
                try:
                    ser.close()
                except Exception:
                    pass
                return


if __name__ == "__main__":
    main()
