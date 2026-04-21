# HDC3022 Linux Driver (ths_hdc3022)

This project implements a Linux kernel module for interfacing with the HDC3022
temperature and humidity sensor over I2C.

## Hardware
- Adafruit HDC3022 T & RH QT (P5989) - [http://adfru.it/5989]
- Adafruit MCP2221A USB-to-GPIO ADC I2C (P4471) - [http://adfru.it/4471]
- STEMMA QT 200mm JST SH 4-Pin Cable (P4401B) - [http://adfru.it/4401]

## Features
- I2C communication via MCP2221A (`/dev/i2c-X`)
- Trigger-on-demand temperature & humidity readings
- CRC-8 validation of sensor data
- Character device interface (`/dev/hdc3022_0`)
- Concurrency-safe: mutex for I2C paths, `atomic_t` for open-file tracking

## Build
```bash
make
```

## Usage

**Blacklist the native HID driver** (required on kernel ≥ 5.7):
```bash
sudo rmmod hid_mcp2221
echo "blacklist hid_mcp2221" | sudo tee /etc/modprobe.d/blacklist.conf
sudo update-initramfs -u
```

**Load the module and instantiate the device** (replace `i2c-X` with your adapter):
```bash
sudo insmod hdc3022.ko
echo hdc3022 0x44 | sudo tee /sys/bus/i2c/devices/i2c-X/new_device
```

**Read sensor data:**
```bash
cat /dev/hdc3022_0
# T=23.456C RH=55.123%
```

**Unload:**
```bash
sudo rmmod hdc3022
```

## References

1. Texas Instruments, *HDC3022 High-Accuracy, Low-Power Humidity Sensor with Temperature Sensor — Datasheet* (SNSS077). Specifies the measurement command (`0x24 0xFF`), soft-reset command (`0x30 0xA2`), raw-to-physical conversion formulas, and CRC-8 parameters (polynomial 0x31, init 0xFF).

2. Adafruit Industries, *Adafruit HDC302x Breakout Board* (ADA5989) — product guide and wiring instructions. https://learn.adafruit.com/adafruit-hdc302x-breakout

3. Adafruit Industries, *MCP2221A USB-to-GPIO ADC I2C* (P4471) — setup guide, including the `hid_mcp2221` blacklist procedure. https://learn.adafruit.com/circuitpython-libraries-on-any-computer-with-mcp2221

4. The Linux Kernel documentation, *I2C/SMBus Subsystem* — `i2c_driver`, `i2c_client`, `i2c_master_send()`/`recv()`, and the `new_device` sysfs interface. https://www.kernel.org/doc/html/latest/i2c/index.html

5. The Linux Kernel documentation, *Character devices* — `alloc_chrdev_region()`, `cdev_init()`, `cdev_add()`, `file_operations`. https://www.kernel.org/doc/html/latest/driver-api/basics.html

6. The Linux Kernel documentation, *Driver Model* — `devm_kzalloc()`, `device_create()`, `class_create()`, and the goto-unwind resource management idiom. https://www.kernel.org/doc/html/latest/driver-api/driver-model/index.html

7. Robert Love, *Linux Kernel Development*, 3rd ed. (Addison-Wesley, 2010) — mutex vs. spinlock selection, `atomic_t` usage, and character driver structure.
