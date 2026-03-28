#!/bin/bash
set -e

echo "[+] USB devices:"
lsusb

echo
echo "[+] I2C adapters:"
i2cdetect -l || true

echo
echo "[+] Recent kernel messages:"
sudo dmesg | tail -n 10
sleep 15
clear