#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include "blue_sensors_ioctl.h"

#define DRIVER_NAME "blue_ak8963"

/* AK8963 Registers */
#define AK8963_REG_WIA   0x00
#define AK8963_REG_HXL   0x03
#define AK8963_REG_ST2   0x09
#define AK8963_REG_CNTL1 0x0A
#define AK8963_REG_ASAX  0x10

struct blue_ak8963_data {
    struct i2c_client *client;
    struct mutex lock;
    struct miscdevice miscdev;
    struct iio_dev *indio_dev;
    u8 asax, asay, asaz;
};

static int ak8963_read_regs(struct i2c_client *client, u8 reg, u8 *buf, int len) {
    struct i2c_msg msgs[2];
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = buf;
    return i2c_transfer(client->adapter, msgs, 2);
}

static int ak8963_write_reg(struct i2c_client *client, u8 reg, u8 val) {
    u8 buf[2] = {reg, val};
    struct i2c_msg msg;
    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = 2;
    msg.buf = buf;
    return i2c_transfer(client->adapter, &msg, 1);
}

/* Character Device Implementation */
static ssize_t blue_ak8963_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct blue_ak8963_data *data = container_of(file->private_data, struct blue_ak8963_data, miscdev);
    u8 raw_data[7]; // 6 data bytes + ST2
    int ret;

    if (count < 7) return -EINVAL;

    mutex_lock(&data->lock);
    /* Re-trigger single measurement (mode 0x11 auto power-downs after each sample) */
    ak8963_write_reg(data->client, AK8963_REG_CNTL1, 0x11);
    msleep(10); /* Wait for measurement to complete (AK8963 datasheet: ~7.2ms) */
    ret = ak8963_read_regs(data->client, AK8963_REG_HXL, raw_data, 7);
    mutex_unlock(&data->lock);

    if (ret < 0) return ret;

    if (copy_to_user(buf, raw_data, 7)) return -EFAULT;

    return 7;
}

static long blue_ak8963_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct blue_ak8963_data *data = container_of(file->private_data, struct blue_ak8963_data, miscdev);
    u8 raw[7];
    int ret;

    switch (cmd) {
    case BLUE_MAG_GET_RAW:
        mutex_lock(&data->lock);
        ret = ak8963_read_regs(data->client, AK8963_REG_HXL, raw, 7);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        if (copy_to_user((void __user *)arg, raw, 7)) return -EFAULT;
        break;
    case BLUE_MAG_GET_ASA:
        mutex_lock(&data->lock);
        raw[0] = data->asax;
        raw[1] = data->asay;
        raw[2] = data->asaz;
        mutex_unlock(&data->lock);
        if (copy_to_user((void __user *)arg, raw, 3)) return -EFAULT;
        break;
    default:
        return -ENOTTY;
    }
    return 0;
}

static const struct file_operations blue_ak8963_fops = {
    .owner = THIS_MODULE,
    .read = blue_ak8963_read,
    .unlocked_ioctl = blue_ak8963_ioctl,
};

/* IIO Implementation */
static int blue_ak8963_read_raw(struct iio_dev *indio_dev,
                               struct iio_chan_spec const *chan,
                               int *val, int *val2, long mask) {
    struct blue_ak8963_data *data = iio_priv(indio_dev);
    u8 raw[7];
    int ret;

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        mutex_lock(&data->lock);
        ret = ak8963_read_regs(data->client, AK8963_REG_HXL, raw, 7);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;

        /* Little-endian */
        if (chan->address == 0) *val = (s16)((raw[1] << 8) | raw[0]);
        else if (chan->address == 1) *val = (s16)((raw[3] << 8) | raw[2]);
        else if (chan->address == 2) *val = (s16)((raw[5] << 8) | raw[4]);
        
        return IIO_VAL_INT;
    default:
        return -EINVAL;
    }
}

static const struct iio_info blue_ak8963_info = {
    .read_raw = blue_ak8963_read_raw,
};

#define AK_CHAN(idx, adr) { \
    .type = IIO_MAGN, \
    .indexed = 1, \
    .channel = idx, \
    .address = adr, \
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
}

static const struct iio_chan_spec blue_ak8963_channels[] = {
    AK_CHAN(0, 0),
    AK_CHAN(1, 1),
    AK_CHAN(2, 2),
};

static int blue_ak8963_probe(struct i2c_client *client) {
    struct blue_ak8963_data *data;
    struct iio_dev *indio_dev;
    u8 raw_asa[3];
    int ret;

    dev_info(&client->dev, "AK8963: Starting probe...\n");

    indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
    if (!indio_dev) return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;
    mutex_init(&data->lock);

    /* Verify device ID (WHO_AM_I) with retries to handle bypass stabilization.
     * Returns -EPROBE_DEFER so the kernel automatically retries this probe
     * after other drivers complete, in case the MPU9250 bypass is not yet active.
     */
    for (ret = 0; ret < 5; ret++) {
        int r = ak8963_read_regs(client, AK8963_REG_WIA, raw_asa, 1);
        if (r >= 0 && raw_asa[0] == 0x48) {
            break;
        }
        dev_warn(&client->dev, "AK8963: Device not ready (WIA=0x%02X), retry %d/5...\n",
                 (r < 0) ? 0 : raw_asa[0], ret + 1);
        msleep(100);
        if (ret == 4) {
            dev_err(&client->dev,
                    "AK8963: Failed after 5 attempts. Deferring probe — ensure blue_mpu9250 I2C bypass is active.\n");
            return -EPROBE_DEFER;
        }
    }

    /**
     * Magnetometer Initialization Procedure:
     * 1. Power down.
     * 2. Fuse ROM access mode.
     * 3. Read sensitivity adjustment values (ASAX, ASAY, ASAZ).
     * 4. Return to power down.
     * 5. Set operational mode (100Hz, 16-bit).
     */
    ak8963_write_reg(client, AK8963_REG_CNTL1, 0x00);
    msleep(10);
    ak8963_write_reg(client, AK8963_REG_CNTL1, 0x0F); // Fuse ROM mode
    msleep(10);
    
    /* i2c_transfer returns number of messages transferred (2), not byte count */
    if (ak8963_read_regs(client, AK8963_REG_ASAX, raw_asa, 3) >= 0) {
        data->asax = raw_asa[0];
        data->asay = raw_asa[1];
        data->asaz = raw_asa[2];
    }

    ak8963_write_reg(client, AK8963_REG_CNTL1, 0x00); // Power down
    msleep(10);
    ak8963_write_reg(client, AK8963_REG_CNTL1, 0x11); // Single measurement mode, 16-bit
    msleep(10);

    /* Register Misc Device (Char Dev) */
    data->miscdev.minor = MISC_DYNAMIC_MINOR;
    data->miscdev.name = "blue-ak8963";
    data->miscdev.fops = &blue_ak8963_fops;
    data->miscdev.parent = &client->dev;
    ret = misc_register(&data->miscdev);
    if (ret) return ret;

    /* Register IIO Device */
    indio_dev->name = DRIVER_NAME;
    indio_dev->info = &blue_ak8963_info;
    indio_dev->channels = blue_ak8963_channels;
    indio_dev->num_channels = ARRAY_SIZE(blue_ak8963_channels);
    indio_dev->modes = INDIO_DIRECT_MODE;

    ret = devm_iio_device_register(&client->dev, indio_dev);
    if (ret) {
        misc_deregister(&data->miscdev);
        return ret;
    }

    i2c_set_clientdata(client, indio_dev);
    dev_info(&client->dev, "Blue AK8963 driver initialized (IIO + Char Dev)\n");
    return 0;
}

static void blue_ak8963_remove(struct i2c_client *client) {
    struct iio_dev *indio_dev = i2c_get_clientdata(client);
    struct blue_ak8963_data *data = iio_priv(indio_dev);
    misc_deregister(&data->miscdev);
}

static const struct i2c_device_id blue_ak8963_id[] = {
    { "blue_ak8963", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, blue_ak8963_id);

static const struct of_device_id blue_ak8963_of_match[] = {
    { .compatible = "blue,ak8963" },
    { }
};
MODULE_DEVICE_TABLE(of, blue_ak8963_of_match);

static struct i2c_driver blue_ak8963_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = blue_ak8963_of_match,
    },
    .probe = blue_ak8963_probe,
    .remove = blue_ak8963_remove,
    .id_table = blue_ak8963_id,
};

module_i2c_driver(blue_ak8963_driver);

MODULE_AUTHOR("BeagleBone Blue BSP Maintainer");
MODULE_DESCRIPTION("BeagleBone Blue AK8963 Magnetometer I2C Driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: blue_mpu9250");
