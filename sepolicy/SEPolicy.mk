# guacamole_pdc_probe (Phase B) sepolicy - scoped to this device ONLY.
#
# Deliberately a separate file from device/oneplus/sm8150-common/sepolicy/
# SEPolicy.mk: sm8150-common is shared by sibling OnePlus devices on this
# platform, and Phase B is explicitly guacamole-only for now (see
# PHASE_B_FINDINGS.md - "sm8150-common 형제 기기에 자동 포함하지 않는다").

BOARD_VENDOR_SEPOLICY_DIRS += device/oneplus/guacamole/sepolicy/vendor
