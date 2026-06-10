# HDC3022 Linux Driver (ths_hdc3022)

Linux kernel module for reading temperature and humidity from the HDC3022
sensor over I2C via a USB-to-I2C bridge.

## Hardware
- Adafruit HDC3022 T & RH QT (P5989) - [http://adafru.it/5989]
- Adafruit MCP2221A USB-to-GPIO ADC I2C (P4471) - [http://adafru.it/4471]
- STEMMA QT 200mm JST SH 4-Pin Cable (P4401B) - [http://adafru.it/4401]

## Build
```bash
make
make clean
```

## One-time setup (kernel ≥ 5.7)

The kernel's built-in MCP2221 driver must be blacklisted or it claims the
device before this driver can reach it:

```bash
sudo rmmod hid_mcp2221
echo "blacklist hid_mcp2221" | sudo tee /etc/modprobe.d/blacklist.conf
sudo update-initramfs -u
# reboot
```

## Load

Replace `i2c-X` with the adapter number shown by `i2cdetect -l`:

```bash
sudo insmod hdc3022.ko
echo hdc3022 0x44 | sudo tee /sys/bus/i2c/devices/i2c-X/new_device
```

## Read temperature

```bash
cat /dev/hdc3022_0
# T=23.456C RH=55.123%
```

## Unload

```bash
sudo rmmod hdc3022
```

## References

1. Texas Instruments, *HDC3022 High-Accuracy, Low-Power Humidity Sensor with Temperature Sensor — Datasheet* (SNSS077). Specifies the measurement command (`0x24 0xFF`), soft-reset command (`0x30 0xA2`), raw-to-physical conversion formulas, and CRC-8 parameters (polynomial 0x31, init 0xFF).

2. Adafruit Industries, *Adafruit HDC302x Breakout Board* (ADA5989) — product guide and wiring instructions. https://learn.adafruit.com/adafruit-hdc302x-breakout

3. Adafruit Industries, *MCP2221A USB-to-GPIO ADC I2C* (P4471) — setup guide, including the `hid_mcp2221` blacklist procedure. https://learn.adafruit.com/circuitpython-libraries-on-any-computer-with-mcp2221 and https://askubuntu.com/questions/110341/how-to-blacklist-kernel-modules

4. The Linux Kernel documentation, *I2C/SMBus Subsystem* — `i2c_driver`, `i2c_client`, `i2c_master_send()`/`recv()`, and the `new_device` sysfs interface. https://www.kernel.org/doc/html/latest/i2c/index.html

5. The Linux Kernel documentation, *Character devices* — `alloc_chrdev_region()`, `cdev_init()`, `cdev_add()`, `file_operations`. https://www.kernel.org/doc/html/latest/driver-api/basics.html

6. The Linux Kernel documentation, *Driver Model* — `device_create()`, `class_create()`, and the goto-unwind resource management idiom. https://www.kernel.org/doc/html/latest/driver-api/driver-model/index.html

7. Robert Love, *Linux Kernel Development*, 3rd ed. (Addison-Wesley, 2010) — mutex vs. spinlock selection, `kref` usage, and character driver structure.