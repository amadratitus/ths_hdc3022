#!/bin/bash

set -e

echo "[+] Updating system..."
sudo apt update && sudo apt upgrade -y

echo "[+] Installing dependencies..."
sudo apt install -y \
    build-essential \
    git \
    linux-headers-$(uname -r) \
    i2c-tools \
    libusb-1.0-0-dev \
    dkms

echo "[+] Enabling I2C tools..."
sudo modprobe i2c-dev || true

echo "[+] Cloning Linux kernel source (reference)..."
if [ ! -d "linux" ]; then
    git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
else
    echo "[!] Linux source already exists, skipping clone."
fi

echo "[+] Creating project build environment..."

mkdir -p build logs

echo "[+] Checking I2C devices..."
i2cdetect -l || echo "[!] No I2C adapters detected"

echo "[+] Setup complete!"
echo "Next steps:"
echo "1. Connect MCP2221A and HDC3022"
echo "2. Run: make"
echo "3. Load module: sudo insmod hdc3022_driver.ko"