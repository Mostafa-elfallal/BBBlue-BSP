#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/math64.h>
#include "blue_sensors_ioctl.h"

#define DRIVER_NAME "blue_bmp280"

/* BMP280 Registers */
#define BMP280_REG_ID         0xD0
#define BMP280_REG_CALIB_START 0x88
#define BMP280_REG_CTRL_MEAS  0xF4
#define BMP280_REG_PRESS_MSB  0xF7
#define BMP280_REG_TEMP_MSB   0xFA

struct bmp280_calib {
    u16 dig_T1; s16 dig_T2, dig_T3;
    u16 dig_P1; s16 dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
};

struct blue_bmp280_data {
    struct i2c_client *client;
    struct mutex lock;
    struct miscdevice miscdev;
    struct iio_dev *indio_dev;
    struct bmp280_calib calib;
    s32 t_fine;
};

static int bmp280_read_regs(struct i2c_client *client, u8 reg, u8 *buf, int len) {
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

static int bmp280_write_reg(struct i2c_client *client, u8 reg, u8 val) {
    u8 buf[2] = {reg, val};
    struct i2c_msg msg;
    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = 2;
    msg.buf = buf;
    return i2c_transfer(client->adapter, &msg, 1);
}

/**
 * @brief Compensates the raw temperature value.
 * @return Temperature in degC * 100 (e.g. 2500 for 25.00 C)
 */
static s32 bmp280_compensate_t(struct blue_bmp280_data *data, s32 adc_T) {
    s32 var1, var2, T;
    var1 = ((((adc_T >> 3) - ((s32)data->calib.dig_T1 << 1))) * ((s32)data->calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((s32)data->calib.dig_T1)) * ((adc_T >> 4) - ((s32)data->calib.dig_T1))) >> 12) * ((s32)data->calib.dig_T3)) >> 14;
    data->t_fine = var1 + var2;
    T = (data->t_fine * 5 + 128) >> 8;
    return T;
}

/**
 * @brief Compensates the raw pressure value.
 * @return Pressure in Pa * 256 (24.8 bit fixed point format)
 */
static u32 bmp280_compensate_p(struct blue_bmp280_data *data, s32 adc_P) {
    s64 var1, var2, p;
    var1 = ((s64)data->t_fine) - 128000;
    var2 = var1 * var1 * (s64)data->calib.dig_P6;
    var2 = var2 + ((var1 * (s64)data->calib.dig_P5) << 17);
    var2 = var2 + (((s64)data->calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (s64)data->calib.dig_P3) >> 8) + ((var1 * (s64)data->calib.dig_P2) << 12);
    var1 = (((((s64)1) << 47) + var1)) * ((s64)data->calib.dig_P1) >> 33;
    if (var1 == 0) return 0;
    p = 1048576 - adc_P;
    p = div_s64(((p << 31) - var2) * 3125, var1);
    var1 = (((s64)data->calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((s64)data->calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((s64)data->calib.dig_P7) << 4);
    return (u32)p;
}

/* Character Device Implementation */
static ssize_t blue_bmp280_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct blue_bmp280_data *data = container_of(file->private_data, struct blue_bmp280_data, miscdev);
    u8 raw_data[6]; // 3 press + 3 temp
    int ret;

    if (count < 6) return -EINVAL;
    if (*ppos > 0) return 0;

    mutex_lock(&data->lock);
    ret = bmp280_read_regs(data->client, BMP280_REG_PRESS_MSB, raw_data, 6);
    mutex_unlock(&data->lock);

    if (ret < 0) return ret;

    if (copy_to_user(buf, raw_data, 6)) return -EFAULT;

    *ppos += 6;
    return 6;
}

static long blue_bmp280_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct blue_bmp280_data *data = container_of(file->private_data, struct blue_bmp280_data, miscdev);
    u8 raw[6];
    s32 adc_T, adc_P;
    int ret;

    switch (cmd) {
    case BLUE_BMP_GET_RAW:
        mutex_lock(&data->lock);
        ret = bmp280_read_regs(data->client, BMP280_REG_PRESS_MSB, raw, 6);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        if (copy_to_user((void __user *)arg, raw, 6)) return -EFAULT;
        break;
    case BLUE_BMP_GET_TEMP:
        mutex_lock(&data->lock);
        ret = bmp280_read_regs(data->client, BMP280_REG_TEMP_MSB, raw, 3);
        if (ret >= 0) {
            adc_T = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
            adc_P = bmp280_compensate_t(data, adc_T); // Reuse adc_P for temp result
        }
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        if (copy_to_user((void __user *)arg, &adc_P, sizeof(s32))) return -EFAULT;
        break;
    case BLUE_BMP_GET_PRESS:
        mutex_lock(&data->lock);
        ret = bmp280_read_regs(data->client, BMP280_REG_TEMP_MSB, raw, 3);
        if (ret >= 0) {
            adc_T = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
            bmp280_compensate_t(data, adc_T);
            ret = bmp280_read_regs(data->client, BMP280_REG_PRESS_MSB, raw, 3);
            if (ret >= 0) {
                adc_P = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
                adc_P = bmp280_compensate_p(data, adc_P); // Pa * 256
            }
        }
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        if (copy_to_user((void __user *)arg, &adc_P, sizeof(u32))) return -EFAULT;
        break;
    default:
        return -ENOTTY;
    }
    return 0;
}

static const struct file_operations blue_bmp280_fops = {
    .owner = THIS_MODULE,
    .read = blue_bmp280_read,
    .unlocked_ioctl = blue_bmp280_ioctl,
};

/* IIO Implementation (Simplified) */
static int blue_bmp280_read_raw(struct iio_dev *indio_dev,
                               struct iio_chan_spec const *chan,
                               int *val, int *val2, long mask) {
    struct blue_bmp280_data *data = iio_priv(indio_dev);
    u8 raw[3];
    int ret;

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        mutex_lock(&data->lock);
        ret = bmp280_read_regs(data->client, chan->address, raw, 3);
        mutex_unlock(&data->lock);
        if (ret < 0) return ret;
        *val = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
        return IIO_VAL_INT;
    default:
        return -EINVAL;
    }
}

static const struct iio_info blue_bmp280_info = {
    .read_raw = blue_bmp280_read_raw,
};

static const struct iio_chan_spec blue_bmp280_channels[] = {
    {
        .type = IIO_PRESSURE,
        .address = BMP280_REG_PRESS_MSB,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
    },
    {
        .type = IIO_TEMP,
        .address = BMP280_REG_TEMP_MSB,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
    },
};

static int blue_bmp280_probe(struct i2c_client *client) {
    struct blue_bmp280_data *data;
    struct iio_dev *indio_dev;
    u8 cal[24];
    int ret;

    indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
    if (!indio_dev) return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;
    mutex_init(&data->lock);

    /* i2c_transfer returns number of messages transferred (2), not byte count */
    if (bmp280_read_regs(client, BMP280_REG_CALIB_START, cal, 24) >= 0) {
        data->calib.dig_T1 = (cal[1] << 8) | cal[0];
        data->calib.dig_T2 = (s16)((cal[3] << 8) | cal[2]);
        data->calib.dig_T3 = (s16)((cal[5] << 8) | cal[4]);
        data->calib.dig_P1 = (cal[7] << 8) | cal[6];
        data->calib.dig_P2 = (s16)((cal[9] << 8) | cal[8]);
        data->calib.dig_P3 = (s16)((cal[11] << 8) | cal[10]);
        data->calib.dig_P4 = (s16)((cal[13] << 8) | cal[12]);
        data->calib.dig_P5 = (s16)((cal[15] << 8) | cal[14]);
        data->calib.dig_P6 = (s16)((cal[17] << 8) | cal[16]);
        data->calib.dig_P7 = (s16)((cal[19] << 8) | cal[18]);
        data->calib.dig_P8 = (s16)((cal[21] << 8) | cal[20]);
        data->calib.dig_P9 = (s16)((cal[23] << 8) | cal[22]);
    }

    bmp280_write_reg(client, BMP280_REG_CTRL_MEAS, 0x27); // Normal mode, 1x osrs

    /* Register Misc Device (Char Dev) */
    data->miscdev.minor = MISC_DYNAMIC_MINOR;
    data->miscdev.name = "blue-bmp280";
    data->miscdev.fops = &blue_bmp280_fops;
    data->miscdev.parent = &client->dev;
    ret = misc_register(&data->miscdev);
    if (ret) return ret;

    /* Register IIO Device */
    indio_dev->name = DRIVER_NAME;
    indio_dev->info = &blue_bmp280_info;
    indio_dev->channels = blue_bmp280_channels;
    indio_dev->num_channels = ARRAY_SIZE(blue_bmp280_channels);
    indio_dev->modes = INDIO_DIRECT_MODE;

    ret = devm_iio_device_register(&client->dev, indio_dev);
    if (ret) {
        misc_deregister(&data->miscdev);
        return ret;
    }

    i2c_set_clientdata(client, indio_dev);
    dev_info(&client->dev, "Blue BMP280 driver initialized (IIO + Char Dev)\n");
    return 0;
}

static void blue_bmp280_remove(struct i2c_client *client) {
    struct iio_dev *indio_dev = i2c_get_clientdata(client);
    struct blue_bmp280_data *data = iio_priv(indio_dev);
    misc_deregister(&data->miscdev);
}

static const struct i2c_device_id blue_bmp280_id[] = {
    { "blue_bmp280", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, blue_bmp280_id);

static const struct of_device_id blue_bmp280_of_match[] = {
    { .compatible = "blue,bmp280" },
    { }
};
MODULE_DEVICE_TABLE(of, blue_bmp280_of_match);

static struct i2c_driver blue_bmp280_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = blue_bmp280_of_match,
    },
    .probe = blue_bmp280_probe,
    .remove = blue_bmp280_remove,
    .id_table = blue_bmp280_id,
};

module_i2c_driver(blue_bmp280_driver);

MODULE_AUTHOR("BeagleBone Blue BSP Maintainer");
MODULE_DESCRIPTION("BeagleBone Blue BMP280 Environmental Sensor Driver");
MODULE_LICENSE("GPL");
