include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=ath79-audio
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/ath79-audio
  SUBMENU:=Sound Support
  TITLE:=AR9341 I2S audio driver (Gramofon / AK4430ET)
  DEPENDS:=@TARGET_ath79 +kmod-sound-core +kmod-sound-soc-core +kmod-sound-soc-simple-card
  FILES:=$(PKG_BUILD_DIR)/ath79-i2s.ko \
         $(PKG_BUILD_DIR)/ak4430et.ko
  AUTOLOAD:=$(call AutoLoad,40,ath79-i2s ak4430et)
endef

define KernelPackage/ath79-audio/description
  ALSA ASoC driver for the AR9341 built-in I2S controller and
  AKM AK4430ET DAC, as found in the Gramofon (Fon FON2415).
endef

MAKE_OPTS:= \
	$(KERNEL_MAKE_FLAGS) \
	M=$(PKG_BUILD_DIR)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

$(eval $(call KernelPackage,ath79-audio))
