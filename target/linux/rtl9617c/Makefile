# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

ARCH:=aarch64
BOARD:=rtl9617c
BOARDNAME:=Realtek RTL9617C (Taurus)
SUBTARGETS:=generic
FEATURES:=fpu nand ramdisk squashfs
CPU_TYPE:=cortex-a53

KERNEL_PATCHVER:=6.18

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += ubi-utils nand-utils ucode-mod-uci

define Target/Description
  Native OpenWrt support for the Realtek RTL9617C (Taurus) SoC family routers
endef

$(eval $(call BuildTarget))
