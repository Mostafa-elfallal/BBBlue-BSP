/*
 * blue-sensor-demo - read the BeagleBone Blue on-board sensors through the
 * character devices created by the blue-sensors kernel modules.
 *
 *   /dev/blue-mpu9250   accelerometer, gyroscope, die temperature
 *   /dev/blue-ak8963    magnetometer
 *   /dev/blue-bmp280    temperature and pressure
 *
 * Usage: blue-sensor-demo [-n samples] [-i interval_ms]
 *   -n 0 reads until interrupted. Defaults: 10 samples, 500 ms.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "blue_sensors_ioctl.h"

#define MPU_DEV "/dev/blue-mpu9250"
#define MAG_DEV "/dev/blue-ak8963"
#define BMP_DEV "/dev/blue-bmp280"

/* Scale factors for the ranges the drivers configure at probe */
#define ACCEL_LSB_PER_G    16384.0  /* ACCEL_CONFIG = 0: +/-2 g      */
#define GYRO_LSB_PER_DPS   131.0    /* GYRO_CONFIG = 0: +/-250 deg/s */
#define MAG_UT_PER_LSB     0.15     /* CNTL1 = 0x11: 16-bit output   */
#define AK8963_ST2_HOFL    0x08     /* magnetic sensor overflow      */

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int16_t be16(const uint8_t *p)
{
	return (int16_t)((p[0] << 8) | p[1]);
}

static int16_t le16(const uint8_t *p)
{
	return (int16_t)((p[1] << 8) | p[0]);
}

static int open_dev(const char *path)
{
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
	return fd;
}

static void print_mpu(int fd)
{
	uint8_t raw[14];

	if (ioctl(fd, BLUE_MPU_GET_ALL, raw) < 0) {
		fprintf(stderr, "mpu9250: %s\n", strerror(errno));
		return;
	}

	/* Registers 0x3B..0x48: accel XYZ, temperature, gyro XYZ (big-endian) */
	printf("accel[g] %7.3f %7.3f %7.3f  gyro[dps] %8.2f %8.2f %8.2f  mpu_t[C] %6.2f\n",
	       be16(&raw[0]) / ACCEL_LSB_PER_G,
	       be16(&raw[2]) / ACCEL_LSB_PER_G,
	       be16(&raw[4]) / ACCEL_LSB_PER_G,
	       be16(&raw[8]) / GYRO_LSB_PER_DPS,
	       be16(&raw[10]) / GYRO_LSB_PER_DPS,
	       be16(&raw[12]) / GYRO_LSB_PER_DPS,
	       be16(&raw[6]) / 333.87 + 21.0);
}

static void print_mag(int fd, const double adj[3])
{
	uint8_t raw[7];
	ssize_t n;

	/* read() starts a single measurement and returns HXL..HZH + ST2 */
	n = read(fd, raw, sizeof(raw));
	if (n != (ssize_t)sizeof(raw)) {
		fprintf(stderr, "ak8963: %s\n", n < 0 ? strerror(errno) : "short read");
		return;
	}
	if (raw[6] & AK8963_ST2_HOFL) {
		printf("mag: overflow\n");
		return;
	}

	printf("mag[uT]  %7.2f %7.2f %7.2f\n",
	       le16(&raw[0]) * adj[0] * MAG_UT_PER_LSB,
	       le16(&raw[2]) * adj[1] * MAG_UT_PER_LSB,
	       le16(&raw[4]) * adj[2] * MAG_UT_PER_LSB);
}

static void print_bmp(int fd)
{
	int32_t temp;
	uint32_t press;

	if (ioctl(fd, BLUE_BMP_GET_TEMP, &temp) < 0 ||
	    ioctl(fd, BLUE_BMP_GET_PRESS, &press) < 0) {
		fprintf(stderr, "bmp280: %s\n", strerror(errno));
		return;
	}

	/* Driver returns degC * 100 and Pa in Q24.8 */
	printf("baro     %6.2f C  %9.2f hPa\n", temp / 100.0, press / 256.0 / 100.0);
}

int main(int argc, char *argv[])
{
	long samples = 10, interval_ms = 500;
	double mag_adj[3] = { 1.0, 1.0, 1.0 };
	int mpu, mag, bmp, opt;

	while ((opt = getopt(argc, argv, "n:i:h")) != -1) {
		switch (opt) {
		case 'n':
			samples = strtol(optarg, NULL, 0);
			break;
		case 'i':
			interval_ms = strtol(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr, "usage: %s [-n samples] [-i interval_ms]\n", argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}
	if (samples < 0 || interval_ms < 0) {
		fprintf(stderr, "samples and interval must not be negative\n");
		return EXIT_FAILURE;
	}

	mpu = open_dev(MPU_DEV);
	mag = open_dev(MAG_DEV);
	bmp = open_dev(BMP_DEV);
	if (mpu < 0 && mag < 0 && bmp < 0)
		return EXIT_FAILURE;

	if (mag >= 0) {
		uint8_t asa[3];

		/* Factory sensitivity adjustment, AK8963 datasheet section 8.3.11 */
		if (ioctl(mag, BLUE_MAG_GET_ASA, asa) == 0)
			for (int i = 0; i < 3; i++)
				mag_adj[i] = (asa[i] - 128) / 256.0 + 1.0;
		else
			fprintf(stderr, "ak8963: no ASA values, using 1.0\n");
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	struct timespec delay = {
		.tv_sec = interval_ms / 1000,
		.tv_nsec = (interval_ms % 1000) * 1000000L,
	};

	for (long i = 0; !stop && (samples == 0 || i < samples); i++) {
		if (i > 0)
			nanosleep(&delay, NULL);
		if (stop)
			break;

		printf("--- sample %ld\n", i + 1);
		if (mpu >= 0)
			print_mpu(mpu);
		if (mag >= 0)
			print_mag(mag, mag_adj);
		if (bmp >= 0)
			print_bmp(bmp);
		fflush(stdout);
	}

	if (mpu >= 0)
		close(mpu);
	if (mag >= 0)
		close(mag);
	if (bmp >= 0)
		close(bmp);
	return EXIT_SUCCESS;
}
