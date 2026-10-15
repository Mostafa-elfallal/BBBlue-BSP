# meta-blue-bsp

Yocto BSP layer for the BeagleBone Blue (TI AM335x, ARM Cortex-A8). It targets headless robotics use: the image keeps networking, USB and audio, drops the display and touchscreen support, and adds kernel drivers for the board's on-board sensors.

## Dependencies

| Layer | Repository | Branch |
|---|---|---|
| `meta` (Poky) | git://git.yoctoproject.org/poky | whinlatter or scarthgap |
| `meta-arm`, `meta-arm-toolchain` | git://git.yoctoproject.org/meta-arm | matching branch |
| `meta-ti-bsp` | git://git.yoctoproject.org/meta-ti | matching branch |

## Building

From the build directory:

```sh
bitbake-layers add-layer ../layers/meta-blue-bsp
```

In `conf/local.conf`:

```sh
MACHINE = "beaglebone-blue"
```

Then:

```sh
bitbake core-image-base
```

The image is written to `tmp/deploy/images/beaglebone-blue/core-image-base-beaglebone-blue.wic.xz`. Write it to a microSD card with `dd` or a tool such as balenaEtcher, then hold the BOOT button while powering on to boot from the card.

## Layer contents

```
conf/
  layer.conf
  machine/beaglebone-blue.conf          machine definition
recipes-kernel/
  linux/
    linux-ti-staging_%.bbappend         device tree and kernel config changes
    files/blue-uart-slots.cfg           kernel config fragment
  blue-sensors/
    blue-sensors-mod_1.0.bb             out-of-tree sensor modules
    files/                              driver sources, ioctl header, Makefile
recipes-apps/
  blue-sensor-demo/
    blue-sensor-demo_1.0.bb             user-space demo for the sensor drivers
    files/blue-sensor-demo.c
```

### Machine configuration

- Based on `ti33x.inc` from meta-ti.
- `MACHINE_FEATURES` limited to `apm usbgadget usbhost vfat ext2 wifi bluetooth alsa`.
- Installs `linux-firmware-wl18xx` for the WL1835MOD Wi-Fi/Bluetooth module. This firmware is distributed by TI under its own licence terms.
- Builds a WIC image (`sdimage-bootpart.wks`) plus a `tar.xz` root filesystem.
- Boots through U-Boot extlinux with the custom device tree, root on `/dev/mmcblk0p2`, console on `ttyS0` at 115200 baud and `earlycon` on UART0.

### Kernel and device tree

The `linux-ti-staging` bbappend fetches the BeagleBoard device tree repository (branch `v6.18.x`) and builds `am335x-boneblue.dts` from it as `ti/omap/custom-blue.dtb`. It adds the `wkup_m3_ipc` firmware property that the BeagleBone Black device tree has, and fixes the `tps65217.dtsi` include paths.

### UART0 console fix

With the default configuration the board stopped printing after `Starting kernel...`. The AM335x has six UARTs, but the kernel allocated only five 8250 port slots (`CONFIG_SERIAL_8250_NR_UARTS=5`). UART0 sits on the `l4_wkup` bus, which is probed after the other UARTs, so it was left without a slot and failed with `unable to register 8250 port`. The console then went to UART5, the GPS header.

`blue-uart-slots.cfg` raises `CONFIG_SERIAL_8250_NR_UARTS` and `CONFIG_SERIAL_8250_RUNTIME_UARTS` to 6, and the `earlycon` argument gives output from UART0 before the serial driver loads.

## Sensor drivers

`blue-sensors-mod` builds three I2C client drivers with `inherit module` and autoloads them at boot.

| Module | Device | Device tree compatible | Character device |
|---|---|---|---|
| `blue_mpu9250` | MPU9250 accelerometer, gyroscope, temperature | `blue,mpu9250` | `/dev/blue-mpu9250` |
| `blue_ak8963` | AK8963 magnetometer | `blue,ak8963` | `/dev/blue-ak8963` |
| `blue_bmp280` | BMP280 temperature and pressure | `blue,bmp280` | `/dev/blue-bmp280` |

Each driver registers an IIO device for raw channel reads and a misc character device. The character device returns raw register data on `read()`. The `ioctl` numbers are defined in `blue_sensors_ioctl.h`; the drivers currently implement:

| Device | `ioctl` | Returns |
|---|---|---|
| MPU9250 | `BLUE_MPU_GET_ALL` | 14 raw bytes: accelerometer XYZ, temperature, gyroscope XYZ |
| AK8963 | `BLUE_MAG_GET_RAW` | 7 raw bytes: XYZ data and ST2 status |
| AK8963 | `BLUE_MAG_GET_ASA` | 3 factory sensitivity adjustment bytes |
| BMP280 | `BLUE_BMP_GET_RAW` | 6 raw bytes: pressure and temperature |
| BMP280 | `BLUE_BMP_GET_TEMP` | Compensated temperature, °C × 100 |
| BMP280 | `BLUE_BMP_GET_PRESS` | Compensated pressure, Pa in Q24.8 |

The remaining numbers in the header are reserved and return `-ENOTTY`.

The AK8963 sits behind the MPU9250. The MPU9250 driver turns on the MPU9250's I2C bypass during probe and then creates the AK8963 client at address `0x0C`. The BMP280 driver reads the factory calibration data during probe and returns compensated temperature and pressure through its `ioctl`s.

## Sensor demo

`blue-sensor-demo` is installed to `/usr/bin` and reads all three sensors through their character devices. It converts the raw values using the ranges the drivers set at probe (±2 g, ±250 °/s, 16-bit magnetometer output) and applies the AK8963 sensitivity adjustment.

```sh
blue-sensor-demo              # 10 samples, 500 ms apart
blue-sensor-demo -n 0 -i 100  # run until Ctrl+C, every 100 ms
```

## License

MIT, see `COPYING.MIT`. The kernel modules declare `MODULE_LICENSE("GPL")`.

## Maintainer

Mostafa ElFallal
