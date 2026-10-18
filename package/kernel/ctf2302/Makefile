# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=ctf2302
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/hwmon-ctf2302
  SUBMENU:=Hardware Monitoring Support
  TITLE:=Sensylink CTF2302 fan controller
  DEPENDS:=+kmod-hwmon-core +kmod-i2c-core
  FILES:=$(PKG_BUILD_DIR)/ctf2302.ko
  AUTOLOAD:=$(call AutoProbe,ctf2302)
endef

define KernelPackage/hwmon-ctf2302/description
  Minimal driver for the Sensylink CTF2302 fan controller found in the
  Ubiquiti UniFi Cloud Gateway Fiber. It sets the chip up the way the
  stock firmware does and exposes three temperatures, two fan speeds and
  the fan duty cycle (pwm1). A small service (ctf2302-fan) sets the duty
  cycle from the hottest temperature.
endef

define KernelPackage/hwmon-ctf2302/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ctf2302-fan $(1)/usr/sbin/ctf2302-fan
	$(INSTALL_BIN) ./files/ctf2302-fan.init $(1)/etc/init.d/ctf2302-fan
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,hwmon-ctf2302))
