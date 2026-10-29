# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

KERNEL_LOADADDR := 0x08000000

define Device/tianyi_tyfza1_2p5g
  DEVICE_VENDOR := Tianyi
  DEVICE_MODEL := TYFZA1
  DEVICE_VARIANT := 2.5GE + 3xGE
  DEVICE_DTS := rtl9617c-tyfza1-2p5g
  DEVICE_DTS_DIR := ../dts
  DEVICE_PACKAGES := fitblk kmod-rtl-ax-ap tyfza1-pon-control luci-app-tyfza1-pon \
	ubi-utils nand-utils uboot-envtools iw wpad-mbedtls wifi-scripts wireless-regdb \
	kmod-gpio-button-hotplug kmod-usb3 kmod-usb-storage kmod-usb-storage-uas \
	kmod-usb-ledtrig-usbport kmod-tyfza1-usb-led block-mount kmod-fs-vfat kmod-fs-exfat \
	kmod-fs-ntfs3 kmod-fs-ext4 kmod-nls-utf8 kmod-nls-cp437 kmod-nls-iso8859-1
  KERNEL := kernel-bin | gzip
  KERNEL_INITRAMFS := kernel-bin | gzip | \
	fit gzip $$(KDIR)/image-rtl9617c-tyfza1-2p5g.dtb with-initrd | pad-to 128k
  KERNEL_INITRAMFS_SUFFIX := -recovery.itb
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.itb
  IMAGE/sysupgrade.itb := append-kernel | \
	fit gzip $$(KDIR)/image-rtl9617c-tyfza1-2p5g.dtb external-static-with-rootfs | \
	pad-rootfs | append-metadata
endef
TARGET_DEVICES += tianyi_tyfza1_2p5g

define Device/tianyi_tyfza1_4ge
  DEVICE_VENDOR := Tianyi
  DEVICE_MODEL := TYFZA1
  DEVICE_VARIANT := 4xGE
  DEVICE_DTS := rtl9617c-tyfza1-4ge
  DEVICE_DTS_DIR := ../dts
  DEVICE_PACKAGES := fitblk kmod-rtl-ax-ap tyfza1-pon-control luci-app-tyfza1-pon \
	ubi-utils nand-utils uboot-envtools iw wpad-mbedtls wifi-scripts wireless-regdb \
	kmod-gpio-button-hotplug kmod-usb3 block-mount kmod-fs-ext4 kmod-nls-utf8
  KERNEL := kernel-bin | gzip
  KERNEL_INITRAMFS := kernel-bin | gzip | \
	fit gzip $$(KDIR)/image-rtl9617c-tyfza1-4ge.dtb with-initrd | pad-to 128k
  KERNEL_INITRAMFS_SUFFIX := -recovery.itb
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.itb
  IMAGE/sysupgrade.itb := append-kernel | \
	fit gzip $$(KDIR)/image-rtl9617c-tyfza1-4ge.dtb external-static-with-rootfs | \
	pad-rootfs | append-metadata
endef
TARGET_DEVICES += tianyi_tyfza1_4ge

define Device/youhua_pt1500_h10_2p5g
  DEVICE_VENDOR := Youhua
  DEVICE_MODEL := PT1500-H10
  DEVICE_VARIANT := 2.5G
  DEVICE_DTS := rtl9617c-pt1500-h10-2p5g
  DEVICE_DTS_DIR := ../dts
  DEVICE_PACKAGES := fitblk tyfza1-pon-control kmod-usb3 block-mount
  KERNEL := kernel-bin | gzip
  KERNEL_INITRAMFS := kernel-bin | gzip | \
	fit gzip $$(KDIR)/image-rtl9617c-pt1500-h10-2p5g.dtb with-initrd | pad-to 128k
  KERNEL_INITRAMFS_SUFFIX := -recovery.itb
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.itb
  IMAGE/sysupgrade.itb := append-kernel | \
	fit gzip $$(KDIR)/image-rtl9617c-pt1500-h10-2p5g.dtb external-static-with-rootfs | \
	pad-rootfs | append-metadata
endef
TARGET_DEVICES += youhua_pt1500_h10_2p5g

$(eval $(call BuildImage))
