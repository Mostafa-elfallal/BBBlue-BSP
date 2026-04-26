SUMMARY = "BeagleBone Blue Sensor Kernel Modules"
DESCRIPTION = "Kernel drivers for MPU9250, AK8963, and BMP280 sensors on the BB Blue."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit module

SRC_URI = "file://blue_mpu9250.c \
           file://blue_ak8963.c \
           file://blue_bmp280.c \
           file://blue_sensors_ioctl.h \
           file://Makefile \
"

S = "${UNPACKDIR}"
UNPACKDIR = "${WORKDIR}/sources"


# The inherit module class automatically adds the necessary build steps
# for out-of-tree kernel modules.

RPROVIDES:${PN} += "kernel-module-blue-mpu9250 kernel-module-blue-ak8963 kernel-module-blue-bmp280"

KERNEL_MODULE_AUTOLOAD += "blue_mpu9250 blue_ak8963 blue_bmp280"
