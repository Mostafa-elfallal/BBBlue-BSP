FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Kernel config fragment: raise 8250 UART port slots from 5 → 6 so UART0 gets a slot
SRC_URI += "file://blue-uart-slots.cfg"
KERNEL_CONFIG_FRAGMENTS:append = " ${UNPACKDIR}/blue-uart-slots.cfg"

# Fetch the official BeagleBoard device trees repository instead of local files
SRC_URI += "git://git.beagleboard.org/beagleboard/BeagleBoard-DeviceTrees.git;protocol=https;branch=v6.18.x;name=bbdt;destsuffix=bbdt"
SRCREV_bbdt = "${AUTOREV}"
SRCREV_FORMAT = "default_bbdt"

# Ensure we don't accidentally append the old custom dtb name
# KERNEL_DEVICETREE is fully defined in the machine conf now.

# Copy the entire BeagleBoard device tree tree into the kernel source before configuring
do_configure:prepend() {
    # Copy ARM device trees (which match the new ti/omap/... structure)
    cp -r ${UNPACKDIR}/bbdt/src/arm/* ${S}/arch/arm/boot/dts/
    
    # Copy any custom dt-bindings needed by these device trees
    cp -r ${UNPACKDIR}/bbdt/include/dt-bindings/* ${S}/include/dt-bindings/ || true
    
    # Use the blue dts but compile it as custom-blue.dts inside ti/omap/
    cp ${UNPACKDIR}/bbdt/src/arm/ti/omap/am335x-boneblue.dts ${S}/arch/arm/boot/dts/ti/omap/custom-blue.dts
    
    # Inject wkup_m3_ipc scale data just like the Black DTB has, to ensure l4_wkup interconnect doesn't crash
    echo '&wkup_m3_ipc { firmware-name = "am335x-bone-scale-data.bin"; };' >> ${S}/arch/arm/boot/dts/ti/omap/custom-blue.dts
    
    # Copy tps65217.dtsi into ti/omap so local includes work natively everywhere
    cp ${S}/arch/arm/boot/dts/tps65217.dtsi ${S}/arch/arm/boot/dts/ti/omap/tps65217.dtsi
    
    # Update all references to tps65217.dtsi to use local include resolution
    find ${S}/arch/arm/boot/dts/ -type f -name "*.dts*" -exec sed -i 's|/include/ "../../tps65217.dtsi"|#include "tps65217.dtsi"|g' {} +
    find ${S}/arch/arm/boot/dts/ -type f -name "*.dts*" -exec sed -i 's|#include "../../tps65217.dtsi"|#include "tps65217.dtsi"|g' {} +
}