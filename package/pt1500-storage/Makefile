# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=pt1500-storage
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/pt1500-storage
  SECTION:=base
  CATEGORY:=Base system
  TITLE:=Youhua PT1500-H10 fip/rootfs_data layout and overlay seed
  DEPENDS:=@TARGET_rtl9617c +ubi-utils +fstools
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
	python3 ./build_overlay_seed.py $(STAGING_DIR_HOST)/bin/mkfs.ubifs $(PKG_BUILD_DIR)/overlay-seed.ubifs.gz
endef

define Package/pt1500-storage/install
	$(INSTALL_DIR) $(1)/lib/preinit $(1)/usr/share/pt1500
	$(INSTALL_DATA) ./files/79_pt1500_storage $(1)/lib/preinit/79_pt1500_storage
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/overlay-seed.ubifs.gz $(1)/usr/share/pt1500/overlay-seed.ubifs.gz
endef

$(eval $(call BuildPackage,pt1500-storage))
