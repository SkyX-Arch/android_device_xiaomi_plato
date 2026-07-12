#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# AAPT
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xxhdpi

# Audio
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/configs/audio/,$(TARGET_COPY_OUT_VENDOR)/etc)

# Dolby
$(call inherit-product-if-exists, hardware/dolby/dolby.mk)

# LunarisDolby
ifneq ($(wildcard hardware/dolby/dolby.mk),)
PRODUCT_PACKAGES += \
    LunarisDolby
endif

# Enabling hardware acceleration for interface rendering
PRODUCT_PROPERTY_OVERRIDES += \
    ro.config.skip_hw_image_decoding=true \
    debug.hwui.renderer=skiavk \
    debug.renderengine.backend=threaded \
    renderthread.skia.reduceopstasksplitting=true \
    ro.dalvik.vm.enable_uffd_gc=false

# Fingerprint
TARGET_HAS_UDFPS := true

# NFC
PRODUCT_PACKAGES += \
    com.android.nfc_extras \
    Tag \
    AxDiagnostics #Fixing an error made by the firmware developers: for some reason, a system service was missing, causing the system to crash.

PRODUCT_PACKAGES += \
    android.hardware.nfc-service.nxp

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.nfc.ese.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.ese.xml \
    frameworks/native/data/etc/android.hardware.nfc.hce.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hce.xml \
    frameworks/native/data/etc/android.hardware.nfc.hcef.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hcef.xml \
    frameworks/native/data/etc/android.hardware.nfc.uicc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.uicc.xml \
    frameworks/native/data/etc/android.hardware.nfc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.se.omapi.ese.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.se.omapi.ese.xml \
    frameworks/native/data/etc/android.hardware.se.omapi.uicc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.se.omapi.uicc.xml \
    frameworks/native/data/etc/com.android.nfc_extras.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.android.nfc_extras.xml \
    frameworks/native/data/etc/com.nxp.mifare.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nxp.mifare.xml

# Overlay
PRODUCT_PACKAGES += \
    FrameworksResOverlayPlato \
    SettingsProviderOverlayPlato \
    SettingsResOverlayPlato \
    SystemUIOverlayPlato \
    WifiResOverlayPlato

PRODUCT_ENFORCE_RRO_TARGETS := *

# Rootdir
PRODUCT_PACKAGES += \
    init.project.rc

# Set support hide display cutout feature
PRODUCT_PRODUCT_PROPERTIES += \
    ro.support_hide_display_cutout=true

PRODUCT_PACKAGES += \
    NoCutoutOverlay \
    AvoidAppsInCutoutOverlay

# Soong
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Shipping API Level
PRODUCT_SHIPPING_API_LEVEL := 31

# Inherit from mt6895-common
$(call inherit-product, device/xiaomi/mt6895-common/mt6895.mk)

# Inherit the proprietary files
$(call inherit-product, vendor/xiaomi/plato/plato-vendor.mk)

# Call the GCAM setup
#$(call inherit-product, vendor/xiaomi/plato-GCAM/plato-GCAM-vendor.mk)
