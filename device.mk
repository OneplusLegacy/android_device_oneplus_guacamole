#
# Copyright (C) 2018-2019 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Enable the complete Settings feature only for this product.
$(call soong_config_set_bool,guacamole_efsd,settings_enabled,true)

# AAPT
PRODUCT_AAPT_CONFIG := xxxhdpi
PRODUCT_AAPT_PREF_CONFIG := xxxhdpi

# Audio
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_platform_info.xml:$(TARGET_COPY_OUT_ODM)/etc/audio_platform_info.xml \
    $(LOCAL_PATH)/audio/mixer_paths.xml:$(TARGET_COPY_OUT_ODM)/etc/mixer_paths.xml \
    $(LOCAL_PATH)/audio/sound_trigger_mixer_paths.xml:$(TARGET_COPY_OUT_ODM)/etc/sound_trigger_mixer_paths.xml

# Boot animation
TARGET_SCREEN_HEIGHT := 3120
TARGET_SCREEN_WIDTH := 1440

# Camera
PRODUCT_PACKAGES += \
    OnePlusCameraHelper

# Device init scripts
PRODUCT_PACKAGES += \
    fstab.qcom \
    fstab.qcom.ramdisk

# guacamole_pdc_probe (Phase B, userdebug/eng dev tool only - see
# device/oneplus/guacamole/pdc_probe/PHASE_B_FINDINGS.md). Deliberately
# added only here, not in sm8150-common's device.mk, so sibling devices
# on this platform do not pick it up.
PRODUCT_PACKAGES_DEBUG += \
    guacamole_pdc_probe \
    guacamole_pdc_mapping_probe \
    guacamole_pdc_carrier_probe

# guacamole_efs_probe (Phase D0, userdebug/eng dev tool only - see
# device/oneplus/guacamole/efs_probe/PHASE_D0_EFS2_RESEARCH.md). Same
# posture as guacamole_pdc_probe above: debug-only, this device only.
PRODUCT_PACKAGES_DEBUG += \
    guacamole_efs_probe \
    guacamole_efs_experiment

# Fastboot
TARGET_BOARD_FASTBOOT_INFO_FILE := $(LOCAL_PATH)/fastboot-info.txt

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay-lineage

PRODUCT_PACKAGES += \
    KeyHandlerResTarget \
    OPlusFrameworksResTarget \
    OPlusSettingsProviderResTarget \
    OPlusSettingsResTarget \
    OPlusSystemUIResTarget

# guacamole_efsd (Cycle 2 Section E/J, W1 real-device smoke test round,
# 2026-09-26). REQUIRED here, not optional: Soong's `init_rc`/
# `vintf_fragments` module properties are only copied into the actual
# built image (out/target/product/.../vendor/etc/{init,vintf/manifest}/)
# when the owning module is reachable from PRODUCT_PACKAGES - building
# `guacamole_efsd` standalone via `m guacamole_efsd` and manually adb
# pushing the binary does NOT install the VINTF manifest fragment,
# which is exactly why servicemanager could not find
# com.guacamole.efsd.IGuacamoleEfsd/default on the real device (status=-3
# abort) even though the daemon itself was running. The daemon's own
# internal state machine still performs zero EFS2/PDC I/O regardless of
# being installed - see guacamole_efsd_service.h's own header comment.
PRODUCT_PACKAGES += \
    guacamole_efsd

# Shipping API
PRODUCT_SHIPPING_API_LEVEL := 28

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Inherit from the common OEM chipset makefile.
$(call inherit-product, device/oneplus/sm8150-common/common.mk)

# Inherit from the proprietary files makefile.
$(call inherit-product, vendor/oneplus/guacamole/guacamole-vendor.mk)

# D1: non-debug production worker and immutable legacy evidence authority.
PRODUCT_PACKAGES += \
    guacamole_efs_worker \
    guacamole_efs_evidence \
    GuacamoleCarrierController
