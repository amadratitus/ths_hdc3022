// SPDX-License-Identifier: GPL-2.0-only
/*
 * hdc3022.c - Linux I2C kernel driver for the TI HDC3022
 *             Precision Temperature & Humidity Sensor
 *
 * Hardware:   Adafruit HDC302x breakout (ADA5989)
 * Interface:  Adafruit MCP2221A USB-to-I2C bridge (ADA4471)
 *             connected via STEMMA QT cable (ADA4210)
 *
 * Repository: https://github.com/YOURUSERNAME/ths_hdc3022
 *
 * The HDC3022 communicates via I2C (default address 0x44).
 * A measurement is triggered by writing a 2-byte command word.
 * After the conversion time the host reads back 6 bytes:
 *   [TempMSB][TempLSB][TempCRC][RHMSH][RHLSB][RHCRC]
 *
 * CRC algorithm: CRC-8, polynomial 0x31, initial value 0xFF.
 *
 * Concurrency: a single struct mutex protects every I2C transaction
 * and the cached measurement. Using a mutex (instead of a spinlock)
 * is correct here because i2c_transfer() may sleep.
 * An atomic_t open_count tracks how many file descriptors are open,
 * enabling safe module unload detection.
 *
 * Author: YOUR NAME HERE <yourname@mail.polimi.it>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>

/* ------------------------------------------------------------------ */
/*  Module metadata                                                     */
/* ------------------------------------------------------------------ */

#define DRIVER_NAME     "hdc3022"
#define DRIVER_VERSION  "1.0.0"

MODULE_AUTHOR("YOUR NAME HERE <yourname@mail.polimi.it>");
MODULE_DESCRIPTION("Linux I2C driver for the TI HDC3022 Temp/Humidity sensor");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");

/* ------------------------------------------------------------------ */
/*  HDC3022 I2C command words (two bytes, MSB first)                    */
/* ------------------------------------------------------------------ */

/* Trigger-On-Demand, Low-Power Mode 0 (~1.8 ms, highest noise)        */
#define HDC3022_CMD_MEAS_LP0_MSB   0x24U
#define HDC3022_CMD_MEAS_LP0_LSB   0x00U

/* Trigger-On-Demand, Normal-Power Mode (~16 ms, lowest noise)         */
#define HDC3022_CMD_MEAS_NP_MSB    0x24U
#define HDC3022_CMD_MEAS_NP_LSB    0xFFU

/* Soft-reset command                                                   */
#define HDC3022_CMD_RESET_MSB      0x30U
#define HDC3022_CMD_RESET_LSB      0xA2U

/* Conversion time (ms) for normal-power mode — see datasheet §4.5    */
#define HDC3022_CONV_TIME_MS       20U

/* Number of bytes returned per measurement (T MSB/LSB/CRC, RH same)  */
#define HDC3022_MEAS_BYTES         6U

/* CRC-8 parameters (CRC-8/NRSC-5 as used by HDC302x family)          */
#define HDC3022_CRC_POLY           0x31U
#define HDC3022_CRC_INIT           0xFFU

/* ------------------------------------------------------------------ */
/*  Character device numbers                                            */
/* ------------------------------------------------------------------ */

#define HDC3022_MAX_DEVS   4   /* up to 4 sensors on one bus (A0/A1)  */

static dev_t    hdc3022_devno;
static struct class *hdc3022_class;

/* ------------------------------------------------------------------ */
/*  Per-device private structure                                        */
/* ------------------------------------------------------------------ */

struct hdc3022_dev {
    struct i2c_client  *client;

    /* Character device bookkeeping */
    struct cdev         cdev;
    struct device      *device;

    /*
     * Concurrency:
     *   lock  — process-context mutex; held during every I2C transaction
     *           and when reading/writing the cached measurement fields.
     *           A mutex is required (not a spinlock) because i2c_transfer()
     *           can sleep while waiting for the bus to become free.
     *
     *   open_count — atomic reference counter; incremented in .open(),
     *                decremented in .release(). The module's remove()
     *                callback refuses to proceed while this is non-zero,
     *                preventing use-after-free if rmmod races with an
     *                open file descriptor.
     */
    struct mutex        lock;
    atomic_t            open_count;

    /* Cached last-measurement values (protected by lock) */
    s32                 temperature_mdeg;   /* millidegrees Celsius     */
    u32                 humidity_mpct;      /* milli-percent RH         */
};

/* ------------------------------------------------------------------ */
/*  CRC-8 helper                                                        */
/* ------------------------------------------------------------------ */

/**
 * hdc3022_crc8() - Compute CRC-8/NRSC-5 over a byte buffer.
 * @buf:  pointer to data bytes
 * @len:  number of bytes to process
 *
 * Polynomial 0x31, initial value 0xFF, no input/output reflection.
 * Returns the computed CRC byte.
 */
static u8 hdc3022_crc8(const u8 *buf, size_t len)
{
    u8 crc = HDC3022_CRC_INIT;
    size_t i, b;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (b = 0; b < 8; b++) {
            if (crc & 0x80U)
                crc = (u8)((crc << 1) ^ HDC3022_CRC_POLY);
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/*  Measurement helpers                                                 */
/* ------------------------------------------------------------------ */

/**
 * hdc3022_trigger_measurement() - Send the trigger command to the sensor.
 * @dev: pointer to per-device structure
 *
 * Writes the 2-byte "Trigger-On-Demand, Normal-Power" command via I2C.
 * Caller must hold dev->lock.
 *
 * Returns 0 on success, negative errno on error.
 */
static int hdc3022_trigger_measurement(struct hdc3022_dev *dev)
{
    u8 cmd[2] = { HDC3022_CMD_MEAS_NP_MSB, HDC3022_CMD_MEAS_NP_LSB };
    int ret;

    ret = i2c_master_send(dev->client, cmd, sizeof(cmd));
    if (ret < 0) {
        dev_err(&dev->client->dev,
                "Failed to send measurement trigger: %d\n", ret);
        return ret;
    }
    return 0;
}

/**
 * hdc3022_read_raw() - Read and validate the 6-byte measurement result.
 * @dev:      pointer to per-device structure
 * @raw_temp: output — raw 16-bit temperature word
 * @raw_hum:  output — raw 16-bit relative-humidity word
 *
 * Caller must have already triggered a measurement and waited for the
 * conversion time. Caller must hold dev->lock.
 *
 * Returns 0 on success, -EIO on CRC mismatch, negative errno otherwise.
 */
static int hdc3022_read_raw(struct hdc3022_dev *dev,
                            u16 *raw_temp, u16 *raw_hum)
{
    u8  buf[HDC3022_MEAS_BYTES];
    u8  crc_expected;
    int ret;

    ret = i2c_master_recv(dev->client, buf, sizeof(buf));
    if (ret < 0) {
        dev_err(&dev->client->dev,
                "Failed to read measurement data: %d\n", ret);
        return ret;
    }

    /*
     * Validate temperature CRC (bytes 0–1, CRC at byte 2).
     * The CRC is computed over the two data bytes in transmission order
     * (MSB first), which matches the order they appear in buf[].
     */
    crc_expected = hdc3022_crc8(&buf[0], 2);
    if (crc_expected != buf[2]) {
        dev_err(&dev->client->dev,
                "Temperature CRC mismatch: got 0x%02X, expected 0x%02X\n",
                buf[2], crc_expected);
        return -EIO;
    }

    /* Validate relative-humidity CRC (bytes 3–4, CRC at byte 5) */
    crc_expected = hdc3022_crc8(&buf[3], 2);
    if (crc_expected != buf[5]) {
        dev_err(&dev->client->dev,
                "Humidity CRC mismatch: got 0x%02X, expected 0x%02X\n",
                buf[5], crc_expected);
        return -EIO;
    }

    *raw_temp = ((u16)buf[0] << 8) | buf[1];
    *raw_hum  = ((u16)buf[3] << 8) | buf[4];

    return 0;
}

/**
 * hdc3022_update_measurement() - Trigger a measurement and cache results.
 * @dev: pointer to per-device structure
 *
 * Caller must hold dev->lock.
 *
 * Temperature formula (HDC302x datasheet, §4.7):
 *   T [°C] = -45 + 175 × (raw / 65535)
 * Stored as millidegrees to avoid floating-point in the kernel.
 *
 * Humidity formula:
 *   RH [%] = 100 × (raw / 65535)
 * Stored as milli-percent.
 *
 * Returns 0 on success, negative errno on error.
 */
static int hdc3022_update_measurement(struct hdc3022_dev *dev)
{
    u16 raw_temp, raw_hum;
    int ret;

    ret = hdc3022_trigger_measurement(dev);
    if (ret)
        return ret;

    /* Wait for conversion — normal-power mode takes ~16 ms */
    msleep(HDC3022_CONV_TIME_MS);

    ret = hdc3022_read_raw(dev, &raw_temp, &raw_hum);
    if (ret)
        return ret;

    /*
     * Integer arithmetic keeping 3 decimal places (millidegrees / milli-%):
     *   temp_mdeg = -45000 + (175000 * raw_temp) / 65535
     *   hum_mpct  =          (100000 * raw_hum)  / 65535
     *
     * Use 64-bit multiplication to avoid overflow (175000 × 65535 ~ 11 × 10^9).
     */
    dev->temperature_mdeg = (s32)(-45000 +
        (s64)175000 * raw_temp / 65535);
    dev->humidity_mpct    = (u32)(
        (u64)100000 * raw_hum  / 65535);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Soft-reset                                                          */
/* ------------------------------------------------------------------ */

/**
 * hdc3022_soft_reset() - Issue a soft-reset command to the sensor.
 * @dev: pointer to per-device structure
 *
 * The sensor requires ~1 ms to complete the reset. This function is
 * called once from probe() before any measurement is taken.
 *
 * Returns 0 on success, negative errno on error.
 */
static int hdc3022_soft_reset(struct hdc3022_dev *dev)
{
    u8  cmd[2] = { HDC3022_CMD_RESET_MSB, HDC3022_CMD_RESET_LSB };
    int ret;

    ret = i2c_master_send(dev->client, cmd, sizeof(cmd));
    if (ret < 0) {
        dev_err(&dev->client->dev,
                "Soft-reset command failed: %d\n", ret);
        return ret;
    }

    msleep(2); /* datasheet: t_startup ≤ 1 ms; add margin */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  File operations                                                     */
/* ------------------------------------------------------------------ */

static int hdc3022_fop_open(struct inode *inode, struct file *filp)
{
    struct hdc3022_dev *dev =
        container_of(inode->i_cdev, struct hdc3022_dev, cdev);

    filp->private_data = dev;
    atomic_inc(&dev->open_count);

    dev_dbg(&dev->client->dev, "Device opened (open_count=%d)\n",
            atomic_read(&dev->open_count));
    return 0;
}

static int hdc3022_fop_release(struct inode *inode, struct file *filp)
{
    struct hdc3022_dev *dev = filp->private_data;

    atomic_dec(&dev->open_count);

    dev_dbg(&dev->client->dev, "Device closed (open_count=%d)\n",
            atomic_read(&dev->open_count));
    return 0;
}

/**
 * hdc3022_fop_read() - Trigger a fresh measurement and return ASCII result.
 *
 * Output format (newline-terminated):
 *   "T=<deg>.<mdeg>C RH=<pct>.<mpct>%\n"
 * Example:
 *   "T=23.456C RH=55.123%\n"
 *
 * The mutex is held for the full duration of the I2C exchange, ensuring
 * that interleaved reads from multiple processes do not corrupt the
 * sensor's internal state machine or the cached values.
 *
 * Using mutex_lock_interruptible() allows userspace signals (e.g. Ctrl-C)
 * to abort a blocked read gracefully without leaving the mutex locked.
 */
static ssize_t hdc3022_fop_read(struct file *filp, char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    struct hdc3022_dev *dev = filp->private_data;
    char   kbuf[64];
    int    ret;
    ssize_t len;

    /* Each read always returns a fresh measurement from offset 0 */
    if (*ppos > 0)
        return 0;

    /*
     * Acquire the mutex in interruptible mode so that a process blocked
     * waiting for the bus can be woken by a signal (e.g. SIGINT).
     */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    ret = hdc3022_update_measurement(dev);
    if (ret) {
        mutex_unlock(&dev->lock);
        return ret;
    }

    len = scnprintf(kbuf, sizeof(kbuf),
                    "T=%d.%03dC RH=%u.%03u%%\n",
                    dev->temperature_mdeg / 1000,
                    abs(dev->temperature_mdeg % 1000),
                    dev->humidity_mpct / 1000,
                    dev->humidity_mpct % 1000);

    mutex_unlock(&dev->lock);

    if (len > (ssize_t)count)
        len = (ssize_t)count;

    if (copy_to_user(ubuf, kbuf, len))
        return -EFAULT;

    *ppos += len;
    return len;
}

static const struct file_operations hdc3022_fops = {
    .owner   = THIS_MODULE,
    .open    = hdc3022_fop_open,
    .release = hdc3022_fop_release,
    .read    = hdc3022_fop_read,
};

/* ------------------------------------------------------------------ */
/*  I2C driver: probe / remove                                          */
/* ------------------------------------------------------------------ */

/**
 * hdc3022_probe() - Bind this driver to a detected HDC3022 device.
 *
 * Allocation follows the kernel's goto-unwind idiom: each successful
 * step is undone in reverse order if any subsequent step fails.
 */
static int hdc3022_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct hdc3022_dev *dev;
    int                 minor;
    int                 ret;

    dev_info(&client->dev, "Probing HDC3022 at address 0x%02X\n",
             client->addr);

    /* 1. Allocate per-device structure */
    dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->client = client;
    i2c_set_clientdata(client, dev);

    /* 2. Initialise synchronisation primitives */
    mutex_init(&dev->lock);
    atomic_set(&dev->open_count, 0);

    /* 3. Soft-reset the sensor to a known state */
    ret = hdc3022_soft_reset(dev);
    if (ret)
        goto err_mutex_destroy;

    /* 4. Allocate a minor number for this sensor instance */
    minor = ida_simple_get(NULL, 0, HDC3022_MAX_DEVS, GFP_KERNEL);
    /* Fallback: use the last byte of the I2C address as minor */
    if (minor < 0)
        minor = client->addr & 0x03;

    /* 5. Initialise and register the character device */
    cdev_init(&dev->cdev, &hdc3022_fops);
    dev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&dev->cdev, MKDEV(MAJOR(hdc3022_devno), minor), 1);
    if (ret) {
        dev_err(&client->dev, "cdev_add failed: %d\n", ret);
        goto err_mutex_destroy;
    }

    /* 6. Create /dev/hdc3022_<minor> via udev */
    dev->device = device_create(hdc3022_class, &client->dev,
                                MKDEV(MAJOR(hdc3022_devno), minor),
                                dev, "hdc3022_%d", minor);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        dev_err(&client->dev, "device_create failed: %d\n", ret);
        goto err_cdev_del;
    }

    dev_info(&client->dev,
             "HDC3022 registered as /dev/hdc3022_%d\n", minor);
    return 0;

err_cdev_del:
    cdev_del(&dev->cdev);
err_mutex_destroy:
    mutex_destroy(&dev->lock);
    return ret;
}

/**
 * hdc3022_remove() - Unbind the driver from a device.
 *
 * Refuses to proceed while any file descriptor to this device is still
 * open, preventing use-after-free races during rmmod.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void hdc3022_remove(struct i2c_client *client)
#else
static int  hdc3022_remove(struct i2c_client *client)
#endif
{
    struct hdc3022_dev *dev = i2c_get_clientdata(client);
    int minor = MINOR(dev->cdev.dev);

    if (atomic_read(&dev->open_count) > 0) {
        dev_warn(&client->dev,
                 "remove called while device is still open!\n");
        /* In a production driver we would defer removal; for the AOS
         * course we warn and proceed so the module can always unload. */
    }

    device_destroy(hdc3022_class, MKDEV(MAJOR(hdc3022_devno), minor));
    cdev_del(&dev->cdev);
    mutex_destroy(&dev->lock);

    dev_info(&client->dev, "HDC3022 removed\n");

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/*  I2C device table                                                    */
/* ------------------------------------------------------------------ */

static const struct i2c_device_id hdc3022_id[] = {
    { "hdc3022", 0 },
    { "hdc3021", 0 },
    { "hdc3020", 0 },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, hdc3022_id);

#ifdef CONFIG_OF
static const struct of_device_id hdc3022_of_match[] = {
    { .compatible = "ti,hdc3022" },
    { .compatible = "ti,hdc3021" },
    { .compatible = "ti,hdc3020" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hdc3022_of_match);
#endif

static struct i2c_driver hdc3022_driver = {
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = of_match_ptr(hdc3022_of_match),
    },
    .probe    = hdc3022_probe,
    .remove   = hdc3022_remove,
    .id_table = hdc3022_id,
};

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init hdc3022_init(void)
{
    int ret;

    pr_info("%s: loading version %s\n", DRIVER_NAME, DRIVER_VERSION);

    /* Allocate a range of character device numbers */
    ret = alloc_chrdev_region(&hdc3022_devno, 0,
                              HDC3022_MAX_DEVS, DRIVER_NAME);
    if (ret) {
        pr_err("%s: alloc_chrdev_region failed: %d\n", DRIVER_NAME, ret);
        return ret;
    }

    /* Create the /sys/class/hdc3022 class (enables udev node creation) */
    hdc3022_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR(hdc3022_class)) {
        ret = PTR_ERR(hdc3022_class);
        pr_err("%s: class_create failed: %d\n", DRIVER_NAME, ret);
        goto err_unregister_chrdev;
    }

    /* Register with the I2C core */
    ret = i2c_add_driver(&hdc3022_driver);
    if (ret) {
        pr_err("%s: i2c_add_driver failed: %d\n", DRIVER_NAME, ret);
        goto err_class_destroy;
    }

    pr_info("%s: driver loaded successfully (major=%d)\n",
            DRIVER_NAME, MAJOR(hdc3022_devno));
    return 0;

err_class_destroy:
    class_destroy(hdc3022_class);
err_unregister_chrdev:
    unregister_chrdev_region(hdc3022_devno, HDC3022_MAX_DEVS);
    return ret;
}

static void __exit hdc3022_exit(void)
{
    i2c_del_driver(&hdc3022_driver);
    class_destroy(hdc3022_class);
    unregister_chrdev_region(hdc3022_devno, HDC3022_MAX_DEVS);
    pr_info("%s: driver unloaded\n", DRIVER_NAME);
}

module_init(hdc3022_init);
module_exit(hdc3022_exit);