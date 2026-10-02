#!/bin/bash
set -e

# Script-relative — works no matter where the repo lives.
REPO_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
WORKSPACE_DIR="${REPO_DIR}/ncs"
SDK_DIR="${ZEPHYR_SDK_DIR:-$HOME}"
SDK_VER="1.0.1"

echo "Starting Zephyr/NCS setup in $WORKSPACE_DIR..."

if [ ! -d "$WORKSPACE_DIR" ]; then
    echo "Initializing west workspace..."
    cd "$REPO_DIR"
    west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.1 ncs
fi

cd "$WORKSPACE_DIR"
echo "Updating west modules (this will take a while)..."
west update
west zephyr-export

echo "Applying local patches..."
cd "$WORKSPACE_DIR/modules/lib/openthread"
git apply ../../../../patches/0001-openthread-force-mtd-discovery.patch || echo "Patch already applied or failed."

echo "Setting up Python virtual environment..."
cd "$REPO_DIR"
python3 -m venv .venv
source .venv/bin/activate

echo "Installing python dependencies..."
pip install west
cd "$WORKSPACE_DIR"
pip install -r zephyr/scripts/requirements.txt
pip install -r nrf/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt

# Install Zephyr SDK if not present
cd "$SDK_DIR"
if [ ! -d "zephyr-sdk-${SDK_VER}" ]; then
    echo "Downloading Zephyr SDK ${SDK_VER} into ${SDK_DIR}..."
    wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz
    tar xf zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz
    rm zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz
    cd zephyr-sdk-${SDK_VER}
    mkdir -p gnu
    wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz
    tar xf toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz -C gnu/
    rm toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz
    ./setup.sh -h -c
fi

echo "Zephyr/NCS setup completed successfully!"
