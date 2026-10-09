# guacamole_pdc_probe (Phase B) sepolicy - scoped to this device ONLY.
#
# Deliberately a separate file from device/oneplus/sm8150-common/sepolicy/
# SEPolicy.mk: sm8150-common is shared by sibling OnePlus devices on this
# platform, and Phase B is explicitly guacamole-only for now (see
# PHASE_B_FINDINGS.md - "sm8150-common 형제 기기에 자동 포함하지 않는다").

BOARD_VENDOR_SEPOLICY_DIRS += device/oneplus/guacamole/sepolicy/vendor

# System-owned manual init controls for core-domain adb-root callers.
# Guacamole private policy only; not exported to vendor policy consumers.
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/oneplus/guacamole/sepolicy/system_ext/private

SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/oneplus/guacamole/sepolicy/system_ext/public

# Context generation does not load te_macros; pass an explicit device-only flag.
BOARD_SEPOLICY_M4DEFS += \
    guacamole_debug_contexts=$(if $(filter userdebug eng,$(TARGET_BUILD_VARIANT)),true,false)
