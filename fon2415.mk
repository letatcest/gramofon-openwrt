
# Fon Fonera Hub FON2415 (Gramofon)
define Device/fon_fon2415
  SOC := ar9341
  DEVICE_VENDOR := Fon
  DEVICE_MODEL := Fonera Hub FON2415
  DEVICE_VARIANT := (Gramofon)
  DEVICE_DTS := qca9341_fon_fon2415
  DEVICE_DTS_DIR := ../dts
  IMAGE_SIZE := 16128k
  IMAGES := sysupgrade.bin factory.bin
  IMAGE/sysupgrade.bin := append-kernel | append-rootfs | pad-rootfs | check-size | append-metadata
  IMAGE/factory.bin    := append-kernel | append-rootfs | pad-rootfs | check-size
  DEVICE_PACKAGES := kmod-ath9k
endef
TARGET_DEVICES += fon_fon2415
