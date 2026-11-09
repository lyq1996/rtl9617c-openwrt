include $(TOPDIR)/rules.mk

LUCI_TITLE:=RTL9617C PON configuration and live status
LUCI_DEPENDS:=+luci-base +luci-theme-argon +luci-i18n-argon-config-zh-cn +luci-mod-network +luci-mod-status +luci-proto-ppp +luci-proto-ipv6 +luci-i18n-base-zh-cn +rpcd-mod-ucode +ucode-mod-fs +rtl9617c-pon-control
LUCI_EXTRA_DEPENDS:=rtl9617c-pon-control (>=0.10.0)
LUCI_PKGARCH:=all
PKG_VERSION:=0.11.0
PKG_RELEASE:=
PKG_LICENSE:=GPL-2.0-or-later
# CSSTidy 1.3 predates CSS variables and responsive grid expressions.
CONFIG_LUCI_CSSTIDY:=

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
