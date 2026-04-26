#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include "blue_sensors_ioctl.h"

#define DRIVER_NAME "blue_mpu9250"

/**
 * MPU9250 Register Definitions
 */
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_GYRO_CONFIG  0x1B
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_INT_PIN_CFG  0x37
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_USER_CTRL    0x6A
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_WHO_AM_I     0x75

struct blue_mpu9250_data {
    struct i2c_client *client;
    struct i2c_client *ak8963_client; /* Dynamically instantiated after bypass enable */
    struct mutex lock;
    struct miscdevice miscdev;
    struct iio_dev *indio_dev;
};

static int mpu9250_read_regs(struct i2c_client *client, u8 reg, u8 *buf, int len) {
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

static int mpu9250_write_reg(struct i2c_client *client, u8 reg, u8 val) {
    u8 buf[2] = {reg, val};
    struct i2c_msg msg;
    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = 2;
    msg.buf = buf;
    return i2c_transfer(client->adapter, &msg, 1);
}

/* Character Device Implementation */
static ssize_t blue_mpu9250_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct blue_mpu9250_data *data = container_of(file->private_data, struct blue_mpu9250_data, miscdev);
    u8 raw_data[14];
    int ret;

    if (count < 14) return -EINVAL;

    mutex_lock(&data->lock);
    ret = mpu9250_read_regs(data->client, MPU_REG_ACCEL_XOUT_H, raw_data, 14);
    mutex_unlock(&data->lock);

    if (ret < 0) return ret;

    if (copy_to_user(buf, raw_data, 14)) return -EFAULT;

    return 14;
}

static long blue_mpu9250_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct blue_mpu9250_data *data = container_of(file->private_data, struct blue_mpu9250_data, miscdev);
    u8 raw[14];
    int ret;

    switch (cmd) {
    case BLUE_MPU_GET_ALL:
        mutex_lock(&data->lock);
        ret = mpu9250_read_regs(data->client, MPU_REG_ACCEL_XOUT_H, raw, 14);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        if (copy_to_user((void __user *)arg, raw, 14)) return -EFAULT;
        break;
    default:
        return -ENOTTY;
    }
    return 0;
}

static const struct file_operations blue_mpu9250_fops = {
    .owner = THIS_MODULE,
    .read = blue_mpu9250_read,
    .unlocked_ioctl = blue_mpu9250_ioctl,
};

/* IIO Implementation */
static int blue_mpu9250_read_raw(struct iio_dev *indio_dev,
                               struct iio_chan_spec const *chan,
                               int *val, int *val2, long mask) {
    struct blue_mpu9250_data *data = iio_priv(indio_dev);
    u8 reg;
    __be16 raw;
    int ret;

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        reg = chan->address;
        mutex_lock(&data->lock);
        ret = i2c_smbus_read_word_swapped(data->client, reg);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        *val = (s16)ret;
        return IIO_VAL_INT;
    default:
        return -EINVAL;
    }
}

static const struct iio_info blue_mpu9250_info = {
    .read_raw = blue_mpu9250_read_raw,
};

#define MPU_CHAN(t, idx, adr, name) { \
    .type = t, \
    .indexed = 1, \
    .channel = idx, \
    .address = adr, \
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
}

static const struct iio_chan_spec blue_mpu9250_channels[] = {
    MPU_CHAN(IIO_ACCEL, 0, MPU_REG_ACCEL_XOUT_H, "accel_x"),
    MPU_CHAN(IIO_ACCEL, 1, MPU_REG_ACCEL_XOUT_H + 2, "accel_y"),
    MPU_CHAN(IIO_ACCEL, 2, MPU_REG_ACCEL_XOUT_H + 4, "accel_z"),
    MPU_CHAN(IIO_TEMP, 0, MPU_REG_ACCEL_XOUT_H + 6, "temp"),
    MPU_CHAN(IIO_ANGL_VEL, 0, MPU_REG_ACCEL_XOUT_H + 8, "gyro_x"),
    MPU_CHAN(IIO_ANGL_VEL, 1, MPU_REG_ACCEL_XOUT_H + 10, "gyro_y"),
    MPU_CHAN(IIO_ANGL_VEL, 2, MPU_REG_ACCEL_XOUT_H + 12, "gyro_z"),
};

static int blue_mpu9250_probe(struct i2c_client *client) {
    struct blue_mpu9250_data *data;
    struct iio_dev *indio_dev;
    int ret;

    indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
    if (!indio_dev) return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;
    mutex_init(&data->lock);

    /**
     * Sensor Configuration Procedure:
     * 1. Hardware Reset.
     * 2. Power management wake-up.
     * 3. Full-scale range setup (Accel/Gyro).
     * 4. Digital Low Pass Filter (DLPF) initialization.
     * 5. I2C Bypass mode enablement for downstream sensor access (AK8963).
     */
    mpu9250_write_reg(client, MPU_REG_PWR_MGMT_1, 0x80); // Reset
    msleep(100);
    mpu9250_write_reg(client, MPU_REG_PWR_MGMT_1, 0x01); // Wake up + Clock PLL
    msleep(100);
    
    mpu9250_write_reg(client, MPU_REG_ACCEL_CONFIG, 0x00);
    mpu9250_write_reg(client, MPU_REG_GYRO_CONFIG, 0x00);
    mpu9250_write_reg(client, MPU_REG_CONFIG, 0x03);
    
    /* Disable I2C Master Mode and enable Bypass */
    mpu9250_write_reg(client, MPU_REG_USER_CTRL, 0x00); // Ensure Master is OFF
    msleep(10);
    mpu9250_write_reg(client, MPU_REG_INT_PIN_CFG, 0x02); // Enable Bypass
    msleep(50); /* Allow I2C bypass to stabilize before AK8963 probes */

    /* Dynamically instantiate the AK8963 magnetometer.
     * The AK8963 is only visible on the I2C bus after the bypass is active,
     * so it cannot be declared as a static DTS device — it must be registered here.
     */
    {
        struct i2c_board_info ak_info = {
            I2C_BOARD_INFO("blue_ak8963", 0x0C),
        };
        data->ak8963_client = i2c_new_client_device(client->adapter, &ak_info);
        if (IS_ERR(data->ak8963_client)) {
            dev_warn(&client->dev, "MPU9250: Failed to instantiate AK8963 client (%ld)\n",
                     PTR_ERR(data->ak8963_client));
            data->ak8963_client = NULL;
        } else {
            dev_info(&client->dev, "MPU9250: AK8963 client instantiated at 0x0C\n");
        }
    }

    /* Register Misc Device (Char Dev) */
    data->miscdev.minor = MISC_DYNAMIC_MINOR;
    data->miscdev.name = "blue-mpu9250";
    data->miscdev.fops = &blue_mpu9250_fops;
    data->miscdev.parent = &client->dev;
    ret = misc_register(&data->miscdev);
    if (ret) return ret;

    /* Register IIO Device */
    indio_dev->name = DRIVER_NAME;
    indio_dev->info = &blue_mpu9250_info;
    indio_dev->channels = blue_mpu9250_channels;
    indio_dev->num_channels = ARRAY_SIZE(blue_mpu9250_channels);
    indio_dev->modes = INDIO_DIRECT_MODE;

    ret = devm_iio_device_register(&client->dev, indio_dev);
    if (ret) {
        misc_deregister(&data->miscdev);
        return ret;
    }

    i2c_set_clientdata(client, indio_dev);
    dev_info(&client->dev, "Blue MPU9250 driver initialized (IIO + Char Dev)\n");
    return 0;
}

static void blue_mpu9250_remove(struct i2c_client *client) {
    struct iio_dev *indio_dev = i2c_get_clientdata(client);
    struct blue_mpu9250_data *data = iio_priv(indio_dev);

    /* Unregister the dynamically instantiated AK8963 client first */
    if (data->ak8963_client)
        i2c_unregister_device(data->ak8963_client);

    /* Disable bypass and power down to ensure clean state on next probe */
    mpu9250_write_reg(client, MPU_REG_INT_PIN_CFG, 0x00);
    mpu9250_write_reg(client, MPU_REG_PWR_MGMT_1, 0x40); // Sleep mode

    misc_deregister(&data->miscdev);
}

static const struct i2c_device_id blue_mpu9250_id[] = {
    { "blue_mpu9250", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, blue_mpu9250_id);

static const struct of_device_id blue_mpu9250_of_match[] = {
    { .compatible = "blue,mpu9250" },
    { }
};
MODULE_DEVICE_TABLE(of, blue_mpu9250_of_match);

static struct i2c_driver blue_mpu9250_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = blue_mpu9250_of_match,
    },
    .probe = blue_mpu9250_probe,
    .remove = blue_mpu9250_remove,
    .id_table = blue_mpu9250_id,
};

module_i2c_driver(blue_mpu9250_driver);

MODULE_AUTHOR("BeagleBone Blue BSP Maintainer");
MODULE_DESCRIPTION("BeagleBone Blue MPU9250 I2C Driver");
MODULE_LICENSE("GPL");
