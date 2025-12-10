// hdc3022.c
// Simple i2c driver for TI HDC3022 - exposes sysfs attributes
// Build as out-of-tree module.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/of_device.h>

#define DRIVER_NAME "hdc3022"
#define HDC3022_I2C_ADDR 0x44
#define CMD_MEASURE_MSB 0x2C
#define CMD_MEASURE_LSB 0x06

struct hdc3022_data {
    struct i2c_client *client;
    struct mutex lock;
    double last_temp;
    double last_humi;
};

static u8 hdc_crc8(const u8 *buf, size_t len)
{
    /* CRC8 with polynomial 0x31, initial 0xFF (TI CRC) */
    u8 crc = 0xFF;
    size_t i, j;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static int hdc_do_measure(struct hdc3022_data *data)
{
    int ret;
    u8 cmd[2] = { CMD_MEASURE_MSB, CMD_MEASURE_LSB };
    u8 buf[6]; /* T(2)+CRC, RH(2)+CRC */

    /* send measurement command */
    ret = i2c_master_send(data->client, cmd, 2);
    if (ret < 0) {
        dev_err(&data->client->dev, "i2c write failed: %d\n", ret);
        return ret;
    }

    /* Wait for conversion (TI suggests ~10-15ms, use 20ms) */
    msleep(20);

    ret = i2c_master_recv(data->client, buf, sizeof(buf));
    if (ret < 0) {
        dev_err(&data->client->dev, "i2c read failed: %d\n", ret);
        return ret;
    } else if (ret != sizeof(buf)) {
        dev_err(&data->client->dev, "unexpected read size %d\n", ret);
        return -EIO;
    }

    /* CRC checks */
    if (hdc_crc8(buf, 2) != buf[2]) {
        dev_err(&data->client->dev, "temperature CRC mismatch\n");
        return -EIO;
    }
    if (hdc_crc8(buf + 3, 2) != buf[5]) {
        dev_err(&data->client->dev, "humidity CRC mismatch\n");
        return -EIO;
    }

    /* Convert */
    {
        u16 raw_t = (buf[0] << 8) | buf[1];
        u16 raw_h = (buf[3] << 8) | buf[4];

        /* Temperature: T = -40 + 165 * raw / 2^16-1 */
        data->last_temp = -40.0 + (165.0 * (double)raw_t) / 65535.0;
        /* Humidity: RH = 100 * raw / 2^16-1 */
        data->last_humi = (100.0 * (double)raw_h) / 65535.0;
    }

    dev_dbg(&data->client->dev, "measured T=%.2f C RH=%.2f%%\n",
            data->last_temp, data->last_humi);

    return 0;
}

/* sysfs show functions */
static ssize_t temperature_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct hdc3022_data *data = dev_get_drvdata(dev);
    int ret;

    mutex_lock(&data->lock);
    ret = hdc_do_measure(data);
    if (ret) {
        mutex_unlock(&data->lock);
        return scnprintf(buf, PAGE_SIZE, "error\n");
    }
    ret = scnprintf(buf, PAGE_SIZE, "%.2f\n", data->last_temp);
    mutex_unlock(&data->lock);
    return ret;
}

static ssize_t humidity_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct hdc3022_data *data = dev_get_drvdata(dev);
    int ret;

    mutex_lock(&data->lock);
    ret = hdc_do_measure(data);
    if (ret) {
        mutex_unlock(&data->lock);
        return scnprintf(buf, PAGE_SIZE, "error\n");
    }
    ret = scnprintf(buf, PAGE_SIZE, "%.2f\n", data->last_humi);
    mutex_unlock(&data->lock);
    return ret;
}

static DEVICE_ATTR_RO(temperature);
static DEVICE_ATTR_RO(humidity);

/* Probe / remove */
static int hdc3022_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct hdc3022_data *data;
    int ret;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev, "i2c functionality not supported\n");
        return -ENODEV;
    }

    data = devm_kzalloc(&client->dev, sizeof(struct hdc3022_data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    mutex_init(&data->lock);
    i2c_set_clientdata(client, data);

    /* create sysfs attributes under device */
    ret = device_create_file(&client->dev, &dev_attr_temperature);
    if (ret) {
        dev_err(&client->dev, "failed to create temperature attr\n");
        return ret;
    }
    ret = device_create_file(&client->dev, &dev_attr_humidity);
    if (ret) {
        device_remove_file(&client->dev, &dev_attr_temperature);
        dev_err(&client->dev, "failed to create humidity attr\n");
        return ret;
    }

    dev_info(&client->dev, "HDC3022 probed at 0x%02x\n", client->addr);

    return 0;
}

static int hdc3022_remove(struct i2c_client *client)
{
    struct hdc3022_data *data = i2c_get_clientdata(client);

    device_remove_file(&client->dev, &dev_attr_temperature);
    device_remove_file(&client->dev, &dev_attr_humidity);

    mutex_destroy(&data->lock);

    dev_info(&client->dev, "HDC3022 removed\n");
    return 0;
}

static const struct of_device_id hdc3022_of_match[] = {
    { .compatible = "ti,hdc3022", },
    { }
};
MODULE_DEVICE_TABLE(of, hdc3022_of_match);

static const struct i2c_device_id hdc3022_id[] = {
    { "hdc3022", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, hdc3022_id);

static struct i2c_driver hdc3022_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = of_match_ptr(hdc3022_of_match),
    },
    .probe = hdc3022_probe,
    .remove = hdc3022_remove,
    .id_table = hdc3022_id,
};

module_i2c_driver(hdc3022_driver);

MODULE_AUTHOR("Your Name <you@example.com>");
MODULE_DESCRIPTION("HDC3022 humidity/temperature sensor driver (simple sysfs)");
MODULE_LICENSE("GPL");