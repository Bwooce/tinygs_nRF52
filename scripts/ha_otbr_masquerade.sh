#!/usr/bin/env bash

# HA OTBR Configuration Tweaks
# This script applies the necessary runtime configuration to the Home Assistant
# OpenThread Border Router (OTBR) to route traffic from the Thread mesh to
# the wider internet (and back).
#
# Since these iptables tweaks don't survive a restart of the add-on or HA OS,
# this script can be run manually or triggered via a Home Assistant automation
# (see docs/HA_OTBR_SETUP.md for automation setup).

# The IP address or hostname of the Home Assistant OS running the OTBR
HA_HOST="${1:-192.168.0.50}"

echo "Applying OTBR Tweaks to HA host: $HA_HOST"

ssh -o StrictHostKeyChecking=no hassio@"$HA_HOST" << 'EOF'
echo "[1/2] Elevating OTBR route preference to high..."
docker exec addon_core_openthread_border_router ot-ctl br routeprf high

echo "[2/2] Adding ip6tables MASQUERADE for Thread ULA (fc00::/7) on end0..."
docker exec addon_core_openthread_border_router sh -c "\
  ip6tables -t nat -C POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE 2>/dev/null || \
  ip6tables -t nat -A POSTROUTING -s fc00::/7 -o end0 -j MASQUERADE"

echo "Checking the applied POSTROUTING rule..."
docker exec addon_core_openthread_border_router ip6tables -t nat -L POSTROUTING -v -n | grep MASQUERADE
EOF

echo "Done."
