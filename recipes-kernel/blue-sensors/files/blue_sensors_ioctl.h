#ifndef BLUE_SENSORS_IOCTL_H
#define BLUE_SENSORS_IOCTL_H

#include <linux/ioctl.h>

struct blue_sensor_data {
    float x, y, z;
};

#define BLUE_SENSORS_MAGIC 'B'

/* MPU9250 IOCTLs */
#define BLUE_MPU_GET_ACCEL _IOR(BLUE_SENSORS_MAGIC, 1, struct blue_sensor_data)
#define BLUE_MPU_GET_GYRO  _IOR(BLUE_SENSORS_MAGIC, 2, struct blue_sensor_data)
#define BLUE_MPU_GET_TEMP  _IOR(BLUE_SENSORS_MAGIC, 3, float)
#define BLUE_MPU_GET_ALL   _IOR(BLUE_SENSORS_MAGIC, 4, uint8_t[14])

/**
 * AK8963 IOCTLs
 */
#define BLUE_MAG_GET_DATA  _IOR(BLUE_SENSORS_MAGIC, 5, struct blue_sensor_data)
#define BLUE_MAG_GET_RAW   _IOR(BLUE_SENSORS_MAGIC, 6, uint8_t[7])

/* BMP280 IOCTLs */
#define BLUE_BMP_GET_TEMP  _IOR(BLUE_SENSORS_MAGIC, 7, int32_t)
#define BLUE_BMP_GET_PRESS _IOR(BLUE_SENSORS_MAGIC, 8, uint32_t)
#define BLUE_BMP_GET_RAW   _IOR(BLUE_SENSORS_MAGIC, 9, uint8_t[6])

#define BLUE_MAG_GET_ASA   _IOR(BLUE_SENSORS_MAGIC, 10, uint8_t[3])

#endif
