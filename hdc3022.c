// SPDX-License-Identifier: GPL-2.0-only
/*
 * hdc3022.c - Linux I2C kernel driver for the TI HDC3022
 *             Precision Temperature & Humidity Sensor
 *
 * Hardware:
 *   - Adafruit HDC3022 breakout
 *   - Adafruit MCP2221A USB-to-I2C bridge
 *   - STEMMA QT / Qwiic cable
 *
 * Repository:
 *   https://github.com/amadratitus/ths_hdc3022
 *
 * Notes:
 *   - Default HDC302x I2C address is typically 0x44
 *   - A single mutex protects every I2C transaction and cached measurement
 *   - This project exposes a simple character device:
 *       /dev/hdc3022_<minor>
 *
 * Reading format:
 *   T=23.456C RH=55.123%
 *
 * Author:
 *   Christopher Amadra Titus - christopheramadra.titus@mail.polimi.it
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/err.h>

#define DRIVER_NAME              "hdc3022"
#define DRIVER_VERSION           "1.0.0"

/* HDC3022 command words */
#define HDC3022_CMD_MEAS_NP_MSB  0x24U
#define HDC3022_CMD_MEAS_NP_LSB  0xFFU

#define HDC3022_CMD_RESET_MSB    0x30U
#define HDC3022_CMD_RESET_LSB    0xA2U

#define HDC3022_CONV_TIME_MS     20U
#define HDC3022_MEAS_BYTES       6U

#define HDC3022_CRC_POLY         0x31U
#define HDC3022_CRC_INIT         0xFFU

#define HDC3022_MAX_DEVS         4

MODULE_AUTHOR("CHRISTOPHER AMADRA TITUS christopheramadra.titus@mail.polimi.it");
MODULE_DESCRIPTION("Linux I2C driver for the TI HDC3022 temperature/humidity sensor");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

static dev_t hdc3022_devno;
static struct class *hdc3022_class;

/* A minor allocator*/
static DEFINE_MUTEX(minor_lock);
static bool minor_in_use[HDC3022_MAX_DEVS];

struct hdc3022_dev {
	struct i2c_client *client;

	struct cdev cdev;
	struct device *device;
	int minor;

	/*
	 * Concurrency:
	 * - lock protects I2C transactions, cached measurements, and removed flag
	 * - ref keeps the struct alive while file descriptors are open; the probe
	 *   path holds one reference, each open() acquires one more
	 *
	 * mutex is correct here because i2c_master_send()/recv() may sleep.
	 */
	struct mutex lock;
	struct kref ref;
	bool removed;

	/* Cached last measurement, protected by lock */
	s32 temperature_mdeg; /* millidegrees Celsius */
	u32 humidity_mpct;    /* milli-percent RH */
};

static int hdc3022_alloc_minor(void)
{
	int i;

	mutex_lock(&minor_lock);
	for (i = 0; i < HDC3022_MAX_DEVS; i++) {
		if (!minor_in_use[i]) {
			minor_in_use[i] = true;
			mutex_unlock(&minor_lock);
			return i;
		}
	}
	mutex_unlock(&minor_lock);

	return -ENOSPC;
}

static void hdc3022_free_minor(int minor)
{
	if (minor < 0 || minor >= HDC3022_MAX_DEVS)
		return;

	mutex_lock(&minor_lock);
	minor_in_use[minor] = false;
	mutex_unlock(&minor_lock);
}

static void hdc3022_dev_release(struct kref *ref)
{
	struct hdc3022_dev *dev = container_of(ref, struct hdc3022_dev, ref);

	mutex_destroy(&dev->lock);
	kfree(dev);
}

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

static int hdc3022_soft_reset(struct hdc3022_dev *dev)
{
	u8 cmd[2] = { HDC3022_CMD_RESET_MSB, HDC3022_CMD_RESET_LSB };
	int ret;

	ret = i2c_master_send(dev->client, cmd, sizeof(cmd));
	if (ret != sizeof(cmd)) {
		dev_err(&dev->client->dev,
			"soft reset failed: ret=%d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	msleep(2);
	return 0;
}

static int hdc3022_trigger_measurement(struct hdc3022_dev *dev)
{
	u8 cmd[2] = { HDC3022_CMD_MEAS_NP_MSB, HDC3022_CMD_MEAS_NP_LSB };
	int ret;

	ret = i2c_master_send(dev->client, cmd, sizeof(cmd));
	if (ret != sizeof(cmd)) {
		dev_err(&dev->client->dev,
			"failed to trigger measurement: ret=%d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int hdc3022_read_raw(struct hdc3022_dev *dev, u16 *raw_temp, u16 *raw_hum)
{
	u8 buf[HDC3022_MEAS_BYTES];
	u8 crc_expected;
	int ret;

	ret = i2c_master_recv(dev->client, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		dev_err(&dev->client->dev,
			"failed to read measurement data: ret=%d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	crc_expected = hdc3022_crc8(&buf[0], 2);
	if (crc_expected != buf[2]) {
		dev_err(&dev->client->dev,
			"temperature CRC mismatch: got=0x%02X expected=0x%02X\n",
			buf[2], crc_expected);
		return -EIO;
	}

	crc_expected = hdc3022_crc8(&buf[3], 2);
	if (crc_expected != buf[5]) {
		dev_err(&dev->client->dev,
			"humidity CRC mismatch: got=0x%02X expected=0x%02X\n",
			buf[5], crc_expected);
		return -EIO;
	}

	*raw_temp = ((u16)buf[0] << 8) | buf[1];
	*raw_hum  = ((u16)buf[3] << 8) | buf[4];

	return 0;
}

static int hdc3022_update_measurement(struct hdc3022_dev *dev)
{
	u16 raw_temp, raw_hum;
	int ret;

	ret = hdc3022_trigger_measurement(dev);
	if (ret)
		return ret;

	msleep(HDC3022_CONV_TIME_MS);

	ret = hdc3022_read_raw(dev, &raw_temp, &raw_hum);
	if (ret)
		return ret;

	dev->temperature_mdeg = (s32)(-45000 +
		((s64)175000 * raw_temp) / 65535);

	dev->humidity_mpct = (u32)(((u64)100000 * raw_hum) / 65535);

	return 0;
}

static int hdc3022_fop_open(struct inode *inode, struct file *filp)
{
	struct hdc3022_dev *dev;

	dev = container_of(inode->i_cdev, struct hdc3022_dev, cdev);
	kref_get(&dev->ref);
	filp->private_data = dev;

	dev_dbg(&dev->client->dev, "opened\n");
	return 0;
}

static int hdc3022_fop_release(struct inode *inode, struct file *filp)
{
	struct hdc3022_dev *dev = filp->private_data;

	kref_put(&dev->ref, hdc3022_dev_release);
	return 0;
}

static ssize_t hdc3022_fop_read(struct file *filp, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct hdc3022_dev *dev = filp->private_data;
	char kbuf[64];
	ssize_t len;
	int ret;
	s32 temp_mdeg;
	u32 hum_mpct;

	if (*ppos > 0)
		return 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	if (dev->removed) {
		mutex_unlock(&dev->lock);
		return -ENODEV;
	}

	ret = hdc3022_update_measurement(dev);
	if (ret) {
		mutex_unlock(&dev->lock);
		return ret;
	}

	temp_mdeg = dev->temperature_mdeg;
	hum_mpct = dev->humidity_mpct;

	len = scnprintf(kbuf, sizeof(kbuf),
		"T=%s%d.%03dC RH=%u.%03u%%\n",
		(temp_mdeg < 0) ? "-" : "",
		abs(temp_mdeg / 1000),
		abs(temp_mdeg % 1000),
		hum_mpct / 1000,
		hum_mpct % 1000);

	mutex_unlock(&dev->lock);

	if (len > count)
		len = count;

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

static int hdc3022_probe(struct i2c_client *client)
{
	struct hdc3022_dev *dev;
	int ret;
	int minor;

	dev_info(&client->dev, "probing HDC3022 at address 0x%02X\n", client->addr);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	dev->minor = -1;
	mutex_init(&dev->lock);
	kref_init(&dev->ref);
	i2c_set_clientdata(client, dev);

	ret = hdc3022_soft_reset(dev);
	if (ret)
		goto err_free;

	minor = hdc3022_alloc_minor();
	if (minor < 0) {
		dev_err(&client->dev, "no free minor numbers\n");
		ret = minor;
		goto err_free;
	}
	dev->minor = minor;

	cdev_init(&dev->cdev, &hdc3022_fops);
	dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&dev->cdev, MKDEV(MAJOR(hdc3022_devno), dev->minor), 1);
	if (ret) {
		dev_err(&client->dev, "cdev_add failed: %d\n", ret);
		goto err_free_minor;
	}

	dev->device = device_create(hdc3022_class, &client->dev,
				    MKDEV(MAJOR(hdc3022_devno), dev->minor),
				    dev, "hdc3022_%d", dev->minor);
	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		dev_err(&client->dev, "device_create failed: %d\n", ret);
		goto err_cdev_del;
	}

	dev_info(&client->dev, "registered as /dev/hdc3022_%d\n", dev->minor);
	return 0;

err_cdev_del:
	cdev_del(&dev->cdev);
err_free_minor:
	hdc3022_free_minor(dev->minor);
	dev->minor = -1;
err_free:
	kref_put(&dev->ref, hdc3022_dev_release);
	return ret;
}

static void hdc3022_remove(struct i2c_client *client)
{
	struct hdc3022_dev *dev = i2c_get_clientdata(client);

	if (!dev)
		return;

	mutex_lock(&dev->lock);
	dev->removed = true;
	mutex_unlock(&dev->lock);

	if (dev->device)
		device_destroy(hdc3022_class,
			       MKDEV(MAJOR(hdc3022_devno), dev->minor));

	cdev_del(&dev->cdev);

	if (dev->minor >= 0)
		hdc3022_free_minor(dev->minor);

	dev_info(&client->dev, "HDC3022 removed\n");

	/* Drop the probe reference. If no fds are open this frees dev;
	 * otherwise the last fop_release will free it. */
	kref_put(&dev->ref, hdc3022_dev_release);
}

static const struct i2c_device_id hdc3022_id[] = {
	{ "hdc3022", 0 },
	{ "hdc3021", 0 },
	{ "hdc3020", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hdc3022_id);

#ifdef CONFIG_OF
static const struct of_device_id hdc3022_of_match[] = {
	{ .compatible = "ti,hdc3022" },
	{ .compatible = "ti,hdc3021" },
	{ .compatible = "ti,hdc3020" },
	{ }
};
MODULE_DEVICE_TABLE(of, hdc3022_of_match);
#endif

static struct i2c_driver hdc3022_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(hdc3022_of_match),
	},
	.probe = hdc3022_probe,
	.remove = hdc3022_remove,
	.id_table = hdc3022_id,
};

static int __init hdc3022_init(void)
{
	int ret;

	pr_info("%s: loading version %s\n", DRIVER_NAME, DRIVER_VERSION);

	ret = alloc_chrdev_region(&hdc3022_devno, 0, HDC3022_MAX_DEVS, DRIVER_NAME);
	if (ret) {
		pr_err("%s: alloc_chrdev_region failed: %d\n", DRIVER_NAME, ret);
		return ret;
	}

	hdc3022_class = class_create(DRIVER_NAME);
	if (IS_ERR(hdc3022_class)) {
		ret = PTR_ERR(hdc3022_class);
		pr_err("%s: class_create failed: %d\n", DRIVER_NAME, ret);
		goto err_unregister_chrdev;
	}

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
