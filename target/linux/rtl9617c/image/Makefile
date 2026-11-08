# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

loadaddr-$(CONFIG_TARGET_rtl9617c_generic) := 0x08000000

define Device/Default
  PROFILES = Default $$(DEVICE_NAME)
  KERNEL_NAME := Image
  KERNEL := kernel-bin | gzip
  KERNEL_LOADADDR = $(loadaddr-y)
  DEVICE_DTS_DIR := ../dts
  KERNEL_INITRAMFS_SUFFIX := -recovery.itb
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.itb
endef

define Device/youhua_pt1500_h10_2p5g
  DEVICE_VENDOR := Youhua
  DEVICE_MODEL := PT1500-H10
  DEVICE_VARIANT := 2.5G
  SUPPORTED_DEVICES += youhua,pt1500-h10-2p5g
  DEVICE_DTS := rtl9617c-pt1500-h10-2p5g
  DEVICE_PACKAGES := fitblk rtl9617c-pon-control luci-app-rtl9617c-pon luci-theme-argon \
	kmod-gpio-button-hotplug luci-ssl luci-i18n-base-zh-cn luci-i18n-firewall-zh-cn
  KERNEL_INITRAMFS := kernel-bin | gzip | \
	fit gzip $$(KDIR)/image-rtl9617c-pt1500-h10-2p5g.dtb with-initrd | pad-to 128k
  IMAGE/sysupgrade.itb := append-kernel | \
	fit gzip $$(KDIR)/image-rtl9617c-pt1500-h10-2p5g.dtb external-static-with-rootfs | \
	pad-rootfs | append-metadata
endef
TARGET_DEVICES += youhua_pt1500_h10_2p5g

define Image/Build
	$(call Image/Build/$(1),$(1))
endef

$(eval $(call BuildImage))
