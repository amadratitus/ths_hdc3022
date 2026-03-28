# HDC3022 Linux Driver (ths_hdc3022)

This project implements a Linux kernel module for interfacing with the HDC3022
temperature and humidity sensor over I2C.

## Hardware
- Adafruit HDC3022 Sensor
- MCP2221A USB-to-I2C bridge
- STEMMA QT cable

## Features
- I2C communication with sensor
- Temperature & humidity readings
- Kernel module implementation
- Concurrency-safe design

## Build
```bash
make