#!/bin/bash
set -e

echo "[+] Unloading hdc3022 module..."
sudo rmmod hdc3022

echo "[+] Last kernel messages:"
sudo dmesg | tail -n 30