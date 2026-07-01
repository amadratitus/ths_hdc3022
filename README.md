# HDC3022 Linux Driver (ths_hdc3022)

Linux kernel module for reading temperature and humidity from the TI HDC3022
sensor over I2C via an MCP2221A USB-to-I2C bridge.

## Hardware

* Adafruit HDC3022 T & RH QT (P5989) — http://adafru.it/5989
* Adafruit MCP2221A USB-to-GPIO ADC I2C (P4471) — http://adafru.it/4471
* STEMMA QT 200mm JST SH 4-Pin Cable (P4401B) — http://adafru.it/4401

The MCP2221A is handled by the kernel's built-in `hid_mcp2221` HID driver,
which exposes it as an I2C adapter (e.g. `/dev/i2c-1`). This driver **must
remain loaded** — do not blacklist it (guides that blacklist `hid_mcp2221`
target userspace/CircuitPython access, which is the opposite use case).

## One-time setup

The mainline kernel ships an IIO driver (`hdc3020`) that matches the same
I2C address (0x44) and will race this driver for the device. Blacklist it:

```sh
sudo modprobe -r hdc3020
echo "blacklist hdc3020" | sudo tee /etc/modprobe.d/blacklist-hdc3020.conf
```

For `i2cdetect` (optional, used to find the bus number):

```sh
sudo apt install i2c-tools
sudo modprobe i2c-dev
```

## Build

```sh
make
make clean
```

## Load

Find the MCP2221 adapter number, then instantiate the sensor
(replace `i2c-X` accordingly):

```sh
sudo i2cdetect -l          # look for "MCP2221 usb-i2c bridge"
sudo insmod hdc3022.ko
echo hdc3022 0x44 | sudo tee /sys/bus/i2c/devices/i2c-X/new_device
```

Alternatively, `./setup.sh` automates dependency installation, bus
detection, build, load, and instantiation.

## Read

```sh
cat /dev/hdc3022_0
# T=23.456C RH=55.123%
```

The device node is world-readable (0444); no root required for reading.

## Unload

```sh
echo 0x44 | sudo tee /sys/bus/i2c/devices/i2c-X/delete_device
sudo rmmod hdc3022
```

## References

1. Texas Instruments, *HDC3022 High-Accuracy, Low-Power Humidity Sensor with
   Temperature Sensor* — datasheet. Specifies the trigger-on-demand
   measurement command, soft-reset command (`0x30 0xA2`), raw-to-physical
   conversion formulas, and CRC-8 parameters (polynomial 0x31, init 0xFF).
2. Adafruit Industries, *Adafruit HDC302x Breakout Board* (ADA5989) — product
   guide and wiring. https://learn.adafruit.com/adafruit-hdc302x-breakout
3. The Linux Kernel documentation, *I2C/SMBus Subsystem* — `i2c_driver`,
   `i2c_client`, `i2c_master_send()`/`recv()`, and the `new_device` sysfs
   interface. https://www.kernel.org/doc/html/latest/i2c/index.html
4. The Linux Kernel documentation, *Character devices* —
   `alloc_chrdev_region()`, `cdev_init()`, `cdev_add()`, `file_operations`.
   https://www.kernel.org/doc/html/latest/driver-api/basics.html
5. The Linux Kernel documentation, *Driver Model* — `device_create()`,
   `class_create()`, and the goto-unwind resource management idiom.
   https://www.kernel.org/doc/html/latest/driver-api/driver-model/index.html
6. Robert Love, *Linux Kernel Development*, 3rd ed. (Addison-Wesley, 2010) —
   mutex vs. spinlock selection, `kref` usage, and character driver structure.
