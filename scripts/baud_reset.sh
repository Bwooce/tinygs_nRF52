#!/bin/bash
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <port> <baudrate>"
    exit 1
fi
python3 scripts/serial_baud_reset.py "$1" "$2"
