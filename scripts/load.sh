#!/bin/bash
set -e

echo "[+] Loading hdc3022 module..."
sudo insmod ./hdc3022.ko

echo "[+] Last kernel messages:"
sudo dmesg | tail -n 30