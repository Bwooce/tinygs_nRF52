#!/usr/bin/env python3
"""Serial monitor that logs to file and stdout."""
import serial
import sys
import time
from datetime import datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
LOGFILE = sys.argv[3] if len(sys.argv) > 3 else "serial.log"

print(f"Waiting for {PORT} ...")

# Wait for port to appear (device reboots after flash)
for i in range(30):
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
        break
    except (serial.SerialException, OSError):
        time.sleep(1)
else:
    print(f"ERROR: {PORT} not available after 30s")
    sys.exit(1)

print(f"Connected to {PORT} @ {BAUD}. Logging to {LOGFILE}")
print("--- Press Ctrl+C to stop ---\n")

with open(LOGFILE, "a") as f:
    f.write(f"\n=== Session started {datetime.now().isoformat()} ===\n")
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
    except KeyboardInterrupt:
        print("\n--- Stopped ---")
    finally:
        ser.close()
