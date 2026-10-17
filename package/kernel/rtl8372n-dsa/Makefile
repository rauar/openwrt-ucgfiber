# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rtl8372n-dsa
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/RuijieNetworksCommunity/rtl837x-dsa-driver.git
PKG_SOURCE_DATE:=2026-10-01
PKG_SOURCE_VERSION:=7f3b64c7f3a37db1866fd80ba815d89974d995fd
PKG_MIRROR_HASH:=c90fa7fcbf9027d6ee5d097442bb40c12a38751a9f66ce8e8b34712cd70173f4

PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define KernelPackage/rtl8372n-dsa
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=Realtek RTL8372N DSA switch driver
  DEPENDS:=@LINUX_6_18 +kmod-dsa
  KCONFIG:= \
	CONFIG_NET_DSA_TAG_RTL8_4=y \
	CONFIG_NET_DSA_TAG_MXL_862XX_8021Q=y
  FILES:=$(PKG_BUILD_DIR)/src/rtl8372n_dsa.ko
  AUTOLOAD:=$(call AutoProbe,rtl8372n_dsa)
endef

define KernelPackage/rtl8372n-dsa/description
  DSA driver for the Realtek RTL8372N 2.5G switch, as found in the
  Ubiquiti UniFi Cloud Gateway Fiber.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)/src" \
		CONFIG_RTL8372N_DSA=m \
		CONFIG_RTL837X_PHY_PATCH=y \
		CONFIG_RTL8372N_DSA_DEBUG=n \
		modules
endef

$(eval $(call KernelPackage,rtl8372n-dsa))
