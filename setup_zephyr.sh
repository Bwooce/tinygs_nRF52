#!/bin/bash
set -e

WORKSPACE_DIR="/home/bruce/dev/tinygs_nRF52/ncs"
SDK_VER="0.16.8"

echo "Starting Zephyr/NCS setup in $WORKSPACE_DIR..."

if [ ! -d "$WORKSPACE_DIR" ]; then
    echo "Initializing west workspace..."
    cd /home/bruce/dev/tinygs_nRF52
    west init -m https://github.com/nrfconnect/sdk-nrf --mr v2.6.0 ncs
fi

cd "$WORKSPACE_DIR"
echo "Updating west modules (this will take a while)..."
west update
west zephyr-export

echo "Installing python dependencies..."
pip3 install -r zephyr/scripts/requirements.txt
pip3 install -r nrf/scripts/requirements.txt
pip3 install -r bootloader/mcuboot/scripts/requirements.txt

# Install Zephyr SDK if not present
cd /home/bruce
if [ ! -d "zephyr-sdk-${SDK_VER}" ]; then
    echo "Downloading Zephyr SDK ${SDK_VER}..."
    wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/zephyr-sdk-${SDK_VER}_linux-x86_64.tar.xz
    tar xf zephyr-sdk-${SDK_VER}_linux-x86_64.tar.xz
    rm zephyr-sdk-${SDK_VER}_linux-x86_64.tar.xz
    cd zephyr-sdk-${SDK_VER}
    ./setup.sh -t arm-zephyr-eabi -h -c
fi

echo "Zephyr/NCS setup completed successfully!"
