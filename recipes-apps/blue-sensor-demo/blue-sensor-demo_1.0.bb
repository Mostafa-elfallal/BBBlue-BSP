SUMMARY = "BeagleBone Blue sensor demo"
DESCRIPTION = "Reads the MPU9250, AK8963 and BMP280 through the blue-sensors character devices and prints the values."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The ioctl header is shared with the kernel modules
FILESEXTRAPATHS:prepend := "${THISDIR}/../../recipes-kernel/blue-sensors/files:"

SRC_URI = "file://blue-sensor-demo.c \
           file://blue_sensors_ioctl.h \
"

S = "${UNPACKDIR}"
UNPACKDIR = "${WORKDIR}/sources"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -o blue-sensor-demo blue-sensor-demo.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 blue-sensor-demo ${D}${bindir}/blue-sensor-demo
}

RRECOMMENDS:${PN} = "blue-sensors-mod"
