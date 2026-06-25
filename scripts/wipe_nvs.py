#!/usr/bin/env python3
import serial
import sys
import time
import argparse

def main():
    parser = argparse.ArgumentParser(description="Trigger configuration wipes on TinyGS nRF52 via USB baud rate.")
    parser.add_argument("port", help="Serial port (e.g., /dev/ttyACM0)")
    parser.add_argument("target", choices=["nvs", "fatfs", "mqtt", "wifi", "all"], 
                        help="Target to wipe: 'nvs' (OpenThread dataset), 'fatfs'/'mqtt'/'wifi' (config.json), or 'all'. Does not default to 'all'.")

    args = parser.parse_args()

    baud_map = {
        "nvs": 1201,     # OpenThread NVS partition
        "fatfs": 1202,   # config.json
        "mqtt": 1202,    # config.json
        "wifi": 1202,    # config.json (nRF52 uses Thread, not WiFi)
        "all": 1203      # Both NVS and config.json
    }

    baud = baud_map[args.target]

    print(f"Setting {args.port} to {baud} baud to wipe {args.target}...")
    try:
        s = serial.Serial(args.port, baud)
        time.sleep(0.5)
        s.close()
        print(f"Done. Device should reboot and wipe {args.target}.")
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
