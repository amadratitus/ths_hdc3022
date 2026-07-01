#!/bin/bash

set -e

# --- Configuration ---
MODULE_NAME="hdc3022"
DEVICE_ADDR=0x44 # Default HDC3022 address
DEV_NODE="/dev/hdc3022_0"
I2C_BUS=""

# --- Cleanup Function ---
cleanup() {
    echo -e "\n[+] Stopping monitoring..."
    stty sane
    tput cnorm # Show cursor
    if lsmod | grep -q "^${MODULE_NAME}"; then
        echo "[+] Unloading kernel module..."
        sudo rmmod ${MODULE_NAME} || true
    fi
    # Remove instantiated device if it exists
    if [ -n "$I2C_BUS" ] && [ -e /sys/bus/i2c/devices/i2c-${I2C_BUS}/new_device ]; then
        echo "${DEVICE_ADDR}" | sudo tee /sys/bus/i2c/devices/i2c-${I2C_BUS}/delete_device > /dev/null 2>&1 || true
    fi
    echo "[+] Cleanup complete."
    exit 0
}

trap cleanup EXIT INT TERM

# --- Step 1: Install Dependencies ---
echo "[+] Installing dependencies..."
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) i2c-tools perl

# --- Step 2: Detect MCP2221A I2C Bus ---
echo "[+] Detecting MCP2221A I2C adapter..."
# Search for 'MCP2221' in the adapter list and extract the bus number (e.g., "i2c-3" -> "3")
I2C_BUS=$(i2cdetect -l | grep -i "mcp2221" | grep -oP 'i2c-\K[0-9]+' | head -n 1)

if [ -z "$I2C_BUS" ]; then
    echo "[!] ERROR: MCP2221A adapter not detected."
    echo "    Please ensure the device is connected and the 'hid_mcp2221' driver is loaded."
    echo "    Run 'i2cdetect -l' manually to verify."
    exit 1
fi

echo "[+] MCP2221A found on I2C bus ${I2C_BUS}"

# --- Step 3: Build Module ---
echo "[+] Building kernel module..."
make clean > /dev/null 2>&1 || true
make
echo "[+] Build successful."

# --- Step 4: Load Prerequisites & Module ---
echo "[+] Loading I2C device interface..."
sudo modprobe i2c-dev || true

echo "[+] Loading ${MODULE_NAME} module..."
sudo insmod ./${MODULE_NAME}.ko
sleep 1

# --- Step 5: Instantiate Device ---
echo "[+] Instantiating HDC3022 on I2C bus ${I2C_BUS}..."
if [ ! -e ${DEV_NODE} ]; then
    echo "${DEVICE_ADDR}" | sudo tee /sys/bus/i2c/devices/i2c-${I2C_BUS}/new_device > /dev/null
    sleep 1
fi

if [ ! -e ${DEV_NODE} ]; then
    echo "[!] Error: Device node ${DEV_NODE} not created. Check dmesg for errors."
    exit 1
fi

echo "[+] Device ready at ${DEV_NODE}"
echo "[+] Starting continuous monitor (Format: T=xx.xxxC RH=xx.xxx%)"
echo "[+] Press ANY key to stop..."

# --- Step 6: Continuous Reading Loop ---
tput civis # Hide cursor
stty -echo # Disable echo

while true; do
    if [ -f ${DEV_NODE} ]; then
        # Read the output: "T=23.456C RH=55.123%"
        output=$(cat ${DEV_NODE} 2>/dev/null)
        
        if [[ -n "$output" ]]; then
            # Display on single line, overwriting previous
            echo -ne "\r\033[K ${output} "
        else
            echo -ne "\r\033[K Waiting for data... "
        fi
    else
        echo -ne "\r\033[K Device node missing... "
    fi
    
    # Wait 2 seconds for key press
    read -t 2 -n 1 -s key
    if [ $? -eq 0 ]; then
        break
    fi
done

# Loop exits, trap runs cleanup   