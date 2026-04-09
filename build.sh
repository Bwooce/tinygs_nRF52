#!/bin/bash
# TinyGS Zephyr Build Script
# Forces Ninja to use 10+ cores to compile faster

set -e

WORKSPACE_DIR="/home/bruce/dev/tinygs_nRF52"

# Activate the virtual environment that has west installed
source "${WORKSPACE_DIR}/.venv/bin/activate"
source "${WORKSPACE_DIR}/ncs/zephyr/zephyr-env.sh"

echo "Building Zephyr App using West with at least 10 cores..."
# Ninja automatically parallelizes to all cores. We can strictly enforce it with an env var.
CMAKE_BUILD_PARALLEL_LEVEL=16 west build -b nrf52840dk_nrf52840 -d build

echo "Build Completed!"
