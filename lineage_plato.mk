#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from plato device
$(call inherit-product, device/xiaomi/plato/device.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/lineage/config/common_full_phone.mk)

# Inherit keys only if they are not included before.
ifndef ANDROID_PRIV_KEYS_MK_INCLUDED
$(call inherit-product, vendor/lineage-priv/keys/keys.mk)
endif

$(call inherit-product, hardware/dolby/dolby.mk)

PRODUCT_MAINLINE_BLUETOOTH_SEPOLICY_DEV_CERTIFICATES := vendor/lineage-priv/keys/

PRODUCT_BRAND := Xiaomi
PRODUCT_DEVICE := plato
PRODUCT_MANUFACTURER := xiaomi
PRODUCT_MODEL := 22071212AG
PRODUCT_NAME := lineage_plato
PRODUCT_SYSTEM_NAME := plato_global
RELEASE_AVD_FLAGS_SET := base
PRODUCT_VIRTUAL_AB_COMPRESSION := true
PRODUCT_VIRTUAL_AB_COMPRESSION_METHOD := zstd
PRODUCT_VIRTUAL_AB_COMPRESSION_FACTOR := 262144

PRODUCT_GMS_CLIENTID_BASE := android-xiaomi

PRODUCT_CHARACTERISTICS := nosdcard

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildFingerprint=Xiaomi/plato_ru/plato:12/SP1A.210812.016/OS2.0.209.0.VLQRUXM:user/release-keys \
    DeviceProduct=$(PRODUCT_SYSTEM_NAME)
