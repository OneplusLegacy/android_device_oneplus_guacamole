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
