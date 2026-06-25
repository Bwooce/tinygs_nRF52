#!/bin/bash
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <user> <ip_address>"
    exit 1
fi

USER=$1
IP=$2
TARGET="$USER@$IP"

echo "Starting commissioner on $TARGET..."
ssh "$TARGET" "sudo docker exec addon_core_openthread_border_router ot-ctl commissioner start"

sleep 1

echo "Adding joiner '*' with PSKd 'TNYGS2026NRF'..."
ssh "$TARGET" "sudo docker exec addon_core_openthread_border_router ot-ctl commissioner joiner add '*' TNYGS2026NRF"

echo "OTBR Commissioning started successfully."
