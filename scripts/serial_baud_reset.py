#!/usr/bin/env python3
import serial
import sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <port> <baudrate>")
    sys.exit(1)

port = sys.argv[1]
baud = int(sys.argv[2])

print(f"Opening {port} at {baud} baud to trigger reset...")
try:
    with serial.Serial(port, baud) as ser:
        pass
    print("Reset triggered.")
except Exception as e:
    print(f"Failed to open port: {e}")
    sys.exit(1)
