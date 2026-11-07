#
# Copyright (C) 2018-2019 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Include the common OEM chipset BoardConfig.
include device/oneplus/sm8150-common/BoardConfigCommon.mk

DEVICE_PATH := device/oneplus/guacamole

# Device-specific stable AIDL service consumed by Settings/controller.
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE += \
    $(DEVICE_PATH)/vintf/framework_compatibility_matrix.xml

# guacamole_pdc_probe (Phase B) sepolicy - guacamole-only, not shared with
# sm8150-common siblings. See PHASE_B_FINDINGS.md.
include $(DEVICE_PATH)/sepolicy/SEPolicy.mk

# Dolby (vendor.dolby_v3_6.hardware.dms360@2.0) HAL sepolicy lives in
# hardware/oplus-guacamole/sepolicy/qti/vendor next to the other Oplus HALs and
# is picked up by hardware/oplus-guacamole/sepolicy/qti/SEPolicy.mk, which
# sm8150-common includes.
#
# The DolbyControl app policy is separate because it belongs to the app port
# rather than the HAL, and it is included here rather than from dolby.mk only
# so the rule exists even when TARGET_ENABLE_ONEPLUS_DOLBY is flipped off after
# the board is configured: BoardConfig has no access to that product variable.
# The rule is inert when the port is disabled because the app is then not
# installed.
#
# Note that setting the variable from a product makefile would work too:
# envsetup.mk evaluates product_config.mk (line 351) before board_config.mk
# (line 367), so product-side additions to BOARD_VENDOR_SEPOLICY_DIRS survive
# into the board phase. That is why vendor/oplus/camera-guacamole's
# SEPolicy.mk is included from opluscamera.mk and still takes effect.
include vendor/oneplus/dolby-guacamole/sepolicy/SEPolicy.mk

# Display
TARGET_SCREEN_DENSITY := 560

# Kernel
BOARD_KERNEL_CMDLINE += androidboot.boot_devices=soc/1d84000.ufshc
TARGET_KERNEL_CONFIG += vendor/oplus_non_dynamic.config

# Partitions
BOARD_BOOTIMAGE_PARTITION_SIZE := 100663296
BOARD_DTBOIMG_PARTITION_SIZE := 25165824
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3640655872
BOARD_USERDATAIMAGE_PARTITION_SIZE := 115601780736
BOARD_VENDORIMAGE_PARTITION_SIZE := 1073741824

# Properties
TARGET_PRODUCT_PROP += $(DEVICE_PATH)/product.prop
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# Recovery
BOARD_USES_RECOVERY_AS_BOOT := true
TARGET_NO_RECOVERY := true
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/init/fstab.qcom

# Include the proprietary files BoardConfig.
include vendor/oneplus/guacamole/BoardConfigVendor.mk
