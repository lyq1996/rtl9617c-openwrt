# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# The signed FIT is built with the TYFZA1 fit tooling (make_ubi_fit.py, the
# boot FIT ABI owner, from the U-Boot-TYFZA1 repository at its pinned commit)
# and the signing key directory. Both come from the environment: populate
# TYFZA1_FIT_TOOLS with a checkout of U-Boot-TYFZA1 tools/fit.
TYFZA1_FIT_TOOLS ?= $(TOPDIR)/tyfza1-fit-tools
TYFZA1_SIGNING_KEYS ?= $(TOPDIR)/tyfza1-signing-keys

define Build/tyfza1-ubi-fit
	@test -f "$(TYFZA1_FIT_TOOLS)/tools/fit/make_ubi_fit.py" || { \
		echo "TYFZA1 fit tools missing; point TYFZA1_FIT_TOOLS at the U-Boot-TYFZA1 checkout" >&2; exit 1; }
	@test -f "$(TYFZA1_SIGNING_KEYS)/dev.key" || { \
		echo "TYFZA1 signing keys missing; point TYFZA1_SIGNING_KEYS at the dev.key/dev.crt directory" >&2; exit 1; }
	python3 $(TYFZA1_FIT_TOOLS)/tools/fit/make_ubi_fit.py \
		$@ $(KDIR)/image-rtl9617c-tyfza1-ubi.dtb $@.fit \
		--dtb-2p5g $(KDIR)/image-rtl9617c-tyfza1-ubi-2p5g.dtb \
		--keys $(TYFZA1_SIGNING_KEYS) \
		--kernel-version $(LINUX_VERSION)
	$(CP) $@.fit/image.itb $@
endef

define Build/tyfza1-dtb
	$(CP) $(KDIR)/image-$(1).dtb $@
endef

define Build/tyfza1-rootfs-size
	@size="$$(stat -c%s $(IMAGE_ROOTFS))"; \
	[ "$$size" -le 33648640 ] || { \
		echo "TYFZA1 rootfs is $$size bytes; OEM rootfs volume holds 33648640 bytes" >&2; \
		exit 1; \
	}
endef

# PT1500 fip: one FIT (kernel + DTB, config "9617c") for U-Boot's fip volume.
define Build/fip-itb
	$(CP) $(KDIR)/vmlinux-initramfs $@.kernel
	$(CP) $(KDIR)/image-rtl9617c-pt1500-h10.dtb $@.dtb
	printf '/dts-v1/;\n/ {\n description = "OpenWrt PT1500-H10 fip";\n #address-cells = <1>;\n images {\n  kernel {\n   description = "OpenWrt initramfs";\n   data = /incbin/("%s");\n   type = "kernel";\n   arch = "arm64";\n   os = "linux";\n   compression = "none";\n   load = <0x08000000>;\n   entry = <0x08000000>;\n  };\n  fdt {\n   description = "PT1500-H10 DTB";\n   data = /incbin/("%s");\n   type = "flat_dt";\n   arch = "arm64";\n   compression = "none";\n   load = <0x0a000000>;\n  };\n };\n configurations {\n  default = "9617c";\n  9617c {\n   description = "PT1500-H10";\n   kernel = "kernel";\n   fdt = "fdt";\n  };\n };\n};\n' "$@.kernel" "$@.dtb" > $@.its
	$(STAGING_DIR_HOST)/bin/mkimage -f $@.its $@
	$(RM) $@.kernel $@.dtb $@.its
endef

define Device/tianyi_tyfza1
  DEVICE_VENDOR := Tianyi
  DEVICE_MODEL := TYFZA1
  SUPPORTED_DEVICES += tianyi,tyfza1-2p5g
  DEVICE_DTS := rtl9617c-tyfza1 rtl9617c-tyfza1-nand-readonly rtl9617c-tyfza1-stock-ubi-readonly rtl9617c-tyfza1-mdio rtl9617c-tyfza1-xram-lan rtl9617c-tyfza1-dma-tx rtl9617c-tyfza1-ni rtl9617c-tyfza1-pon-status rtl9617c-tyfza1-pcie rtl9617c-tyfza1-pcie-2p5g rtl9617c-tyfza1-ubi rtl9617c-tyfza1-ubi-2p5g
  DEVICE_DTS_DIR := ../dts
  IMAGE_METADATA := "tyfza1_storage": "factory-v1"
  DEVICE_PACKAGES := kmod-rtl-ax-ap tyfza1-pon-control tyfza1-storage luci-app-tyfza1-pon \
	ubi-utils nand-utils uboot-envtools iw wpad-mbedtls wifi-scripts wireless-regdb \
	kmod-gpio-button-hotplug kmod-usb3 kmod-usb-storage kmod-usb-storage-uas \
	kmod-usb-ledtrig-usbport kmod-tyfza1-usb-led block-mount kmod-fs-vfat kmod-fs-exfat \
	kmod-fs-ntfs3 kmod-fs-ext4 kmod-nls-utf8 kmod-nls-cp437 kmod-nls-iso8859-1
  KERNEL := kernel-bin | tyfza1-ubi-fit
  KERNEL_INITRAMFS := kernel-bin
  KERNEL_SUFFIX := -kernel.itb
  KERNEL_INITRAMFS_SUFFIX := -initramfs-Image
  KERNEL_INSTALL := 1
  FILESYSTEMS := squashfs
  IMAGES := rootfs.squashfs sysupgrade.bin
  IMAGE/rootfs.squashfs := append-rootfs | tyfza1-rootfs-size
  IMAGE/sysupgrade.bin := tyfza1-rootfs-size | sysupgrade-tar | append-metadata
  ARTIFACTS := board.dtb
  ifdef TYFZA1_BRINGUP_ARTIFACTS
  # Bring-up and validation DTBs for the probe flows and the U-Boot-side
  # offline validators; enable with TYFZA1_BRINGUP_ARTIFACTS=1.
  ARTIFACTS += nand-readonly.dtb stock-ubi-readonly.dtb mdio-probe.dtb xram-lan.dtb dma-tx.dtb ni.dtb pon-status.dtb pcie.dtb pcie-2p5g.dtb ubi.dtb ubi-2p5g.dtb
  endif
  ARTIFACT/board.dtb := tyfza1-dtb rtl9617c-tyfza1
  ARTIFACT/nand-readonly.dtb := tyfza1-dtb rtl9617c-tyfza1-nand-readonly
  ARTIFACT/stock-ubi-readonly.dtb := tyfza1-dtb rtl9617c-tyfza1-stock-ubi-readonly
  ARTIFACT/mdio-probe.dtb := tyfza1-dtb rtl9617c-tyfza1-mdio
  ARTIFACT/xram-lan.dtb := tyfza1-dtb rtl9617c-tyfza1-xram-lan
  ARTIFACT/dma-tx.dtb := tyfza1-dtb rtl9617c-tyfza1-dma-tx
  ARTIFACT/ni.dtb := tyfza1-dtb rtl9617c-tyfza1-ni
  ARTIFACT/pon-status.dtb := tyfza1-dtb rtl9617c-tyfza1-pon-status
  ARTIFACT/pcie.dtb := tyfza1-dtb rtl9617c-tyfza1-pcie
  ARTIFACT/pcie-2p5g.dtb := tyfza1-dtb rtl9617c-tyfza1-pcie-2p5g
  ARTIFACT/ubi.dtb := tyfza1-dtb rtl9617c-tyfza1-ubi
  ARTIFACT/ubi-2p5g.dtb := tyfza1-dtb rtl9617c-tyfza1-ubi-2p5g
endef
# TARGET_DEVICES += tianyi_tyfza1  # TYFZA1 needs external fit tools; PT1500-only build

# PT1500-H10: the system image is one FIT (kernel + DTB + initramfs root) in
# the fip UBI volume, booted by U-Boot with configuration 9617c. Persistence
# comes from the rootfs_data UBIFS volume (package pt1500-storage), so the
# root filesystem is the initramfs and no squashfs volume is needed.
define Device/youhua_pt1500_h10
  DEVICE_VENDOR := Youhua
  DEVICE_MODEL := PT1500-H10
  DEVICE_DTS := rtl9617c-pt1500-h10
  DEVICE_DTS_DIR := ../dts
  # luci-proto-ppp carries both PPP and PPPoE in this feed; an IPv4 static or
  # DHCP protocol needs no extra package on top of luci-mod-network.
  DEVICE_PACKAGES := kmod-usb3 block-mount pt1500-storage \
	kmod-gpio-button-hotplug \
	luci luci-ssl luci-proto-ppp \
	uhttpd uhttpd-mod-ubus luci-app-firewall firewall4 dnsmasq
  KERNEL := kernel-bin
  KERNEL_INITRAMFS := kernel-bin
  KERNEL_INITRAMFS_SUFFIX := -initramfs-Image
  KERNEL_INSTALL := 1
  FILESYSTEMS := squashfs
  IMAGES := fip.itb
  IMAGE/fip.itb := fip-itb
  ARTIFACTS := board.dtb
  ARTIFACT/board.dtb := tyfza1-dtb rtl9617c-pt1500-h10
endef
TARGET_DEVICES += youhua_pt1500_h10

$(eval $(call BuildImage))

