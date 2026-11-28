# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Device/Default
  PROFILES = $$(DEVICE_NAME)
  KERNEL_NAME := Image
  KERNEL := kernel-bin | gzip
  KERNEL_LOADADDR := 0x08000000
  KERNEL_ENTRY := 0x08000000
  DEVICE_DTS_DIR := ../dts
  KERNEL_INITRAMFS_SUFFIX := -recovery.itb
  KERNEL_INITRAMFS = kernel-bin | gzip | \
	      fit gzip $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb with-initrd | pad-to 128k
  FILESYSTEMS := squashfs
  IMAGES := sysupgrade.itb
  IMAGE/sysupgrade.itb = append-kernel | \
	      fit gzip $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb external-static-with-rootfs | \
	      pad-rootfs | append-metadata
endef

define Device/youhua_pt1500_h10_2p5g
  DEVICE_VENDOR := Youhua
  DEVICE_MODEL := PT1500-H10
  DEVICE_VARIANT := 2.5G
  SUPPORTED_DEVICES += youhua,pt1500-h10-2p5g
  DEVICE_DTS := rtl9617c-pt1500-h10-2p5g
  DEVICE_PACKAGES := fitblk rtl9617c-pon-control luci-app-rtl9617c-pon luci-app-rtl9617c-qos-v2 luci-theme-argon \
	kmod-gpio-button-hotplug luci-i18n-base-zh-cn luci-i18n-firewall-zh-cn luci-mod-system \
	uhttpd uhttpd-mod-ubus iperf3 ethtool ip-full tc curl htop openssl-util mtr socat \
	conntrack-tools
endef
TARGET_DEVICES += youhua_pt1500_h10_2p5g

define Device/tianyi_tyfza1_2p5g
  DEVICE_VENDOR := Tianyi
  DEVICE_MODEL := TYFZA1
  DEVICE_VARIANT := 2.5G
  SUPPORTED_DEVICES += tianyi,tyfza1-2p5g
  DEVICE_DTS := rtl9617c-tianyi-tyfza1-2p5g
  DEVICE_PACKAGES := fitblk rtl9617c-pon-control luci-app-rtl9617c-pon luci-app-rtl9617c-qos-v2 luci-theme-argon \
	kmod-gpio-button-hotplug luci-i18n-base-zh-cn luci-i18n-firewall-zh-cn luci-mod-system \
	uhttpd uhttpd-mod-ubus iperf3 ethtool ip-full tc curl htop openssl-util mtr socat \
	conntrack-tools wpad-basic-mbedtls kmod-rtw89-8852ce rtl8852ce-ap-firmware \
	kmod-rtw89-8192xbe
endef
TARGET_DEVICES += tianyi_tyfza1_2p5g

$(eval $(call BuildImage))
