#!/bin/sh
# Verify the PT1500-H10 build products. Run inside WSL:
#   bash /mnt/f/routerCoding/pt1500-h10/rtl9617c-openwrt/pt1500-verify.sh
D=/root/owrt/build_dir/target-aarch64_cortex-a55_musl/linux-rtl9617c_generic
K=$D/linux-6.18.55
R=/root/owrt/build_dir/target-aarch64_cortex-a55_musl/root-rtl9617c

echo '--- kernel objects ---'
ls -l --time-style=+%H:%M "$K/drivers/net/ethernet/realtek/rtl9617c-xram.o" \
	"$K/drivers/net/mdio/mdio-rtl9617c.o" "$K/drivers/net/phy/realtek/realtek_main.o"
ls -l --time-style=+%H:%M "$D/Image" "$D/Image-initramfs" "$D/image-rtl9617c-pt1500-h10.dtb"

echo '--- dtb: ni and mdio ---'
"$K/scripts/dtc/dtc" -I dtb -O dts "$D/image-rtl9617c-pt1500-h10.dtb" 2>/dev/null > /tmp/pt1500.dts
sed -n '/ethernet@4f4300000/,/^\t};/p' /tmp/pt1500.dts
sed -n '/ethernet-phy@7/,/};/p' /tmp/pt1500.dts

echo '--- rootfs ---'
grep anon_mount "$R/etc/config/fstab"
grep -n 'ucidef_set_interface_lan' "$R/etc/board.d/02_network"
ls -l "$R/lib/preinit/79_pt1500_storage" "$R/etc/uci-defaults/95-pt1500-storage" \
	"$R/lib/upgrade/pt1500-fip.sh" | awk '{print $5, $9}'
gunzip -c "$R/usr/share/pt1500/overlay-seed.ubifs.gz" | od -An -tx1 -N4

echo '--- image ---'
ls -l --block-size=K /root/owrt/bin/targets/rtl9617c/generic/*fip.itb
