#!/bin/sh
# Verify the PT1500-H10 build products. Run inside WSL:
#   bash /mnt/f/routerCoding/pt1500-h10/rtl9617c-openwrt/pt1500-verify.sh
D=/root/owrt/build_dir/target-aarch64_cortex-a55_musl/linux-rtl9617c_generic
K=$D/linux-6.18.55
R=/root/owrt/build_dir/target-aarch64_cortex-a55_musl/root-rtl9617c

echo '--- kernel objects ---'
ls -l --time-style=+%H:%M "$K/drivers/net/ethernet/realtek/rtl9617c-xram.o" \
	"$K/drivers/net/mdio/mdio-rtl9617c.o" "$D/Image-initramfs" \
	"$D/image-rtl9617c-pt1500-h10.dtb"

echo '--- dtb: ni ---'
"$K/scripts/dtc/dtc" -I dtb -O dts "$D/image-rtl9617c-pt1500-h10.dtb" 2>/dev/null > /tmp/pt1500.dts
sed -n '/ethernet@4f4300000/,/interrupts = /p' /tmp/pt1500.dts | grep -E 'p6-serdes|dma-tx|qm-rx|lan-phys|phys = <0x'
sed -n '/ethernet-phy@6/,/};/p' /tmp/pt1500.dts

echo '--- rootfs ---'
ls "$R/lib/preinit/" | grep pt1500
ls -l "$R/etc/serdes/" 2>&1
grep -c . "$R/etc/serdes/p6_serdes_sgmii_2g5.txt" 2>&1
grep -n 'ucidef_set_interface_lan\|ucidef_set_led_netdev' "$R/etc/board.d/02_network" "$R/etc/board.d/01_leds"

echo '--- image ---'
ls -l --block-size=K /root/owrt/bin/targets/rtl9617c/generic/*fip.itb
