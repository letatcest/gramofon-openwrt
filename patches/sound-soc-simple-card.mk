
define KernelPackage/sound-soc-simple-card
  TITLE:=Simple Audio Card support
  DEPENDS:=+kmod-sound-soc-core
  KCONFIG:= \
	CONFIG_SND_SIMPLE_CARD_UTILS \
	CONFIG_SND_SIMPLE_CARD
  FILES:= \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card-utils.ko \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card.ko
  AUTOLOAD:=$(call AutoLoad,56,snd-soc-simple-card-utils snd-soc-simple-card)
  $(call AddDepends/sound)
endef

define KernelPackage/sound-soc-simple-card/description
  Simple Audio Card machine driver for device tree simple-audio-card
  binding. Connects CPU DAI and codec DAI as described in DTS.
endef

$(eval $(call KernelPackage,sound-soc-simple-card))
