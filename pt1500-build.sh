#!/bin/sh
# Sync this tree into the WSL build tree, enable the PT1500-H10 packages and
# build. Run inside WSL:
#
#   bash /mnt/f/routerCoding/pt1500-h10/rtl9617c-openwrt/pt1500-build.sh
#
# The build tree keeps its .config and build_dir; this script only copies the
# working tree over it (no --delete) because the build tree also holds
# generated files that the repository does not track.
set -e
SRC=/mnt/f/routerCoding/pt1500-h10/rtl9617c-openwrt
DST=/root/owrt
FORCE_UNSAFE_CONFIGURE=1
export FORCE_UNSAFE_CONFIGURE

rsync -a "$SRC/target/linux/rtl9617c/" "$DST/target/linux/rtl9617c/"
# base-files must mirror the tree exactly: a leftover preinit script there is
# sourced by /etc/preinit and can abort the whole preinit chain.
rsync -a --delete "$SRC/target/linux/rtl9617c/base-files/" "$DST/target/linux/rtl9617c/base-files/"
mkdir -p "$DST/package/pt1500-storage"
rsync -a "$SRC/package/pt1500-storage/" "$DST/package/pt1500-storage/"
cp "$SRC/package/base-files/Makefile" "$DST/package/base-files/Makefile"
echo SYNCED

cd "$DST"
for sym in \
	luci luci-ssl luci-proto-ppp luci-app-firewall \
	uhttpd uhttpd-mod-ubus firewall4 dnsmasq pt1500-storage
do
	if grep -q "^# CONFIG_PACKAGE_$sym is not set" .config; then
		sed -i "s|^# CONFIG_PACKAGE_$sym is not set|CONFIG_PACKAGE_$sym=y|" .config
	elif ! grep -q "^CONFIG_PACKAGE_$sym=" .config; then
		echo "CONFIG_PACKAGE_$sym=y" >> .config
	fi
done
# Upstream renamed the board to youhua_pt1500_h10_2p5g and now ships a FIT
# with a squashfs root (sysupgrade.itb) plus a recovery initramfs; make the
# build tree select that device instead of the removed youhua_pt1500_h10.
sed -i '/^CONFIG_TARGET_rtl9617c_generic_DEVICE_youhua_pt1500_h10=y$/d' .config
sed -i '/^CONFIG_TARGET_PROFILE="DEVICE_youhua_pt1500_h10"$/d' .config
grep -q '^CONFIG_TARGET_rtl9617c_generic_DEVICE_youhua_pt1500_h10_2p5g=y$' .config ||
	echo 'CONFIG_TARGET_rtl9617c_generic_DEVICE_youhua_pt1500_h10_2p5g=y' >> .config

# uboot-envtools' host fw_printenv is not built for this target and fails
# world; the fip/fit slot switch is done over the serial console instead.
sed -i 's|^CONFIG_PACKAGE_uboot-envtools=y|# CONFIG_PACKAGE_uboot-envtools is not set|' .config

make defconfig >/tmp/pt1500-defconfig.log 2>&1 || { tail -20 /tmp/pt1500-defconfig.log; exit 1; }
echo CONFIGURED

# Stale rootfs leftovers from earlier experiments: nothing installs them now,
# and each one costs initramfs space.
rm -f "$DST/build_dir/target-aarch64_cortex-a55_musl/root-rtl9617c/usr/share/pt1500/rootfs_data.ubifs"

# base-files and the board package are file-only, but their stamps do not
# watch target/linux/<board>/base-files, so force them.
make package/base-files/clean package/pt1500-storage/clean >/dev/null 2>&1 || true
make -j"$(nproc)" > "$DST/pt1500-build.log" 2>&1
echo "BUILD OK:"
ls -l --block-size=K "$DST"/bin/targets/rtl9617c/generic/*.itb 2>/dev/null
