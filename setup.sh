#!/bin/bash

set -e

echo "[+] Installing dependencies..."
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) i2c-tools

echo "[+] Loading I2C device interface..."
sudo modprobe i2c-dev || true

echo "[+] Checking I2C adapters..."
i2cdetect -l || echo "[!] No I2C adapters detected"

echo "[+] Done. Connect the MCP2221A and HDC3022, then run: make"
