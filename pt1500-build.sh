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
make defconfig >/tmp/pt1500-defconfig.log 2>&1 || { tail -20 /tmp/pt1500-defconfig.log; exit 1; }
echo CONFIGURED

# base-files and the board package are file-only, but their stamps do not
# watch target/linux/<board>/base-files, so force them.
make package/base-files/clean package/pt1500-storage/clean >/dev/null 2>&1 || true
make -j"$(nproc)" > "$DST/pt1500-build.log" 2>&1
echo "BUILD OK: $(ls "$DST"/bin/targets/rtl9617c/generic/*fip.itb)"
