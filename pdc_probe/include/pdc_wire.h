// guacamole_pdc_probe - QMI PDC (Persistent Device Configuration) wire
// format: message IDs, TLV IDs, and request/response/indication codecs.
//
// SOURCE OF TRUTH FOR THIS FILE:
// Qualcomm does not publish the PDC IDL header (no
// "persistent_device_config_v01.h" exists in this AOSP/PixelOS/OnePlus
// checkout - confirmed by a full-tree search in a prior research pass).
// Instead of guessing the wire layout from disassembly of the stripped
// `pdc_qmi_idl_service_object_v01` object in this device's
// libqmiservices.so (which the Phase A research pass explicitly flagged
// BLOCKED without a decompiler), this file is built from
// linux-mobile-broadband/libqmi's PUBLIC, actively-maintained,
// independently-implemented QMI PDC service definition:
//   https://github.com/linux-mobile-broadband/libqmi
//     data/qmi-service-pdc.json   (message/TLV IDs and formats)
//     data/qmi-common.json        (the universal "Operation Result" TLV)
//     src/libqmi-glib/qmi-enums-pdc.h (QmiPdcConfigurationType values)
// libqmi is used in production by ModemManager against real Qualcomm
// modems across a huge range of devices; its PDC support has shipped
// since libqmi 1.18. This is the same class of evidence Codex's review
// used for EFS2 (qtools/EfsTools) instead of guessing offsets from a
// single stripped sample binary - a public, cross-device-validated wire
// spec beats a one-off disassembly guess.
//
// What IS confirmed by direct binary evidence against THIS firmware
// (H.41, see research_firmware_qmi_pdc_analysis.md):
//   - `pdc_get_service_object_internal_v01` and `pdc_qmi_idl_service_object_v01`
//     are real, exported symbols in vendor/lib64/libqmiservices.so.
//   - QCCI's public client API (`qmi_client_init_instance`,
//     `qmi_client_release`, `qmi_client_send_raw_msg_sync`, ...) is
//     exported from vendor/lib64/libqmi_cci.so with the exact same
//     names as Qualcomm's own public qmi-framework `qmi_cci.h` (see
//     qcci_shim.h for the full transport story, including why
//     libqmi_cci.so and not libqcci_legacy.so).
// What is NOT independently confirmed against this exact firmware build
// (documented honestly, not glossed over):
//   - Whether this device's PDC service implementation is wire-compatible
//     with the "since 1.18" / "since 1.32" message shapes below (Qualcomm
//     does version its QMI services; a mismatch would show up as a
//     non-zero Operation Result error code or a decode failure, both of
//     which this probe reports distinctly rather than silently misparsing).
//   - The exact (major, minor, tool) version triple to pass into
//     `pdc_get_service_object_internal_v01` (see qcci_shim.h).
//
// Phase B scope: List Configs (0x24), Get Config Info (0x28), and
// (Phase B2) Get Selected Config (0x22) - all pure reads. Load Config
// (0x26, Phase C0) is a separate, explicitly-gated write path - see
// pdc_load.h. Activate/Deactivate/Delete/Set Selected Config are still
// NOT implemented anywhere in this codebase.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qmi_tlv.h"

namespace guacamole_pdc {

// QMI_PROTOCOL_ERROR_NO_ENTRY, from libqmi's public
// src/libqmi-glib/qmi-errors.h (`QMI_PROTOCOL_ERROR_NO_ENTRY = 50,
// /*< nick=NoEntry >*/`). PDC defines no service-specific error codes
// of its own (confirmed: src/libqmi-glib/qmi-enums-pdc.h has only
// QmiPdcConfigurationType/QmiPdcRefreshEventType, no error enum), so it
// uses this generic QmiProtocolError set for its Indication Result
// error codes. This is the best publicly-evidenced candidate for "no
// such config ID" - NOT byte-confirmed against this specific device's
// actual observed error_status value for a nonexistent-ID lookup (a gap
// noted honestly, not glossed over). The fail-closed design in
// ClassifyConfigPresence() means a wrong guess here only makes the tool
// MORE conservative (treats a real "not found" as kUnknown -> refuses
// to load) rather than less safe (never misclassifies kUnknown as
// kAbsent and proceeds).
constexpr uint16_t kQmiProtocolErrorNoEntry = 50;

// --- Message IDs (data/qmi-service-pdc.json) ---
constexpr uint32_t kMsgGetSelectedConfig = 0x0022;
constexpr uint32_t kMsgListConfigs = 0x0024;
constexpr uint32_t kMsgGetConfigInfo = 0x0028;

// --- Universal QMI TLV IDs (data/qmi-common.json, data/qmi-service-pdc.json) ---
// IMPORTANT: a TLV id is only meaningful within the context of a single
// message ID - the same numeric id (e.g. 0x11) is reused with a
// DIFFERENT meaning by different PDC messages. Every constant below is
// named for the exact message it belongs to; never share one across
// messages even when the numeric value happens to match.
constexpr uint8_t kTlvOperationResult = 0x02;    // every sync response
constexpr uint8_t kTlvIndicationResult = 0x01;   // every PDC indication
constexpr uint8_t kTlvToken = 0x10;              // request+indication, all PDC msgs
constexpr uint8_t kTlvGetSelectedConfigReqConfigType = 0x01;  // "Config Type" common-ref
constexpr uint8_t kTlvGetSelectedConfigIndActiveId = 0x11;
constexpr uint8_t kTlvGetSelectedConfigIndPendingId = 0x12;
constexpr uint8_t kTlvListConfigsReqConfigType = 0x11;
constexpr uint8_t kTlvListConfigsIndConfigs = 0x11;
constexpr uint8_t kTlvConfigTypeAndId = 0x01;    // Get Config Info request
constexpr uint8_t kTlvGetConfigInfoIndTotalSize = 0x11;
constexpr uint8_t kTlvGetConfigInfoIndDescription = 0x12;
constexpr uint8_t kTlvGetConfigInfoIndVersion = 0x13;

// --- QmiPdcConfigurationType (src/libqmi-glib/qmi-enums-pdc.h) ---
enum class ConfigType : uint32_t {
  kPlatform = 0,
  kSoftware = 1,  // the Korea MBNs are software configs.
};

// Defensive caps - not part of the wire spec, but this probe's own
// trust boundary. QMI_DMS_MAX_CONFIG_LIST_LEN_V01-style caps are normal
// practice in every Qualcomm-generated IDL header (see
// device_management_service_v01.h in this tree); PDC has no published
// equivalent, so these are chosen conservatively:
//   kMaxConfigs: generous headroom over the handful of real configs this
//     device is expected to hold (baseline + up to a few Korea MBNs).
//   kMaxIdLen: cross-checked against `QcRilHook.smali`'s own
//     `MAX_PDC_ID_LEN = 0x7c` (124) constant, found in this exact
//     firmware's qcrilhook.jar (see research_iqtioemhook_pdc_mapping.md).
//     We also independently cap at the wire format's hard ceiling of 255
//     (a guint8 size prefix cannot exceed that).
constexpr size_t kMaxConfigs = 64;
constexpr size_t kMaxIdLen = 124;
constexpr size_t kMaxDescriptionLen = 512;

struct ConfigEntry {
  ConfigType type = ConfigType::kSoftware;
  std::vector<uint8_t> id;
};

struct ListConfigsResult {
  bool ok = false;
  uint16_t error_status = 0;
  uint16_t error_code = 0;
  std::vector<ConfigEntry> configs;
  bool truncated = false;  // true if the wire reported more than kMaxConfigs
};

struct GetConfigInfoResult {
  bool ok = false;
  uint16_t error_status = 0;
  uint16_t error_code = 0;
  uint32_t total_size = 0;
  bool has_total_size = false;
  std::string description;
  bool has_description = false;
  uint32_t version = 0;
  bool has_version = false;
};

// Active/pending config IDs are variable-length binary values (same
// guint8-size-prefixed array format as everywhere else in PDC) - never
// assume a fixed length (e.g. 20 bytes just because that's what the
// Common-Commercial config's ID happened to be on the real device this
// was tested against).
struct GetSelectedConfigResult {
  bool ok = false;
  uint16_t error_status = 0;
  uint16_t error_code = 0;
  std::optional<std::vector<uint8_t>> active_id;   // absent = NONE
  std::optional<std::vector<uint8_t>> pending_id;  // absent = NONE
};

// Builds the TLV body for a Get Selected Config request.
std::vector<uint8_t> BuildGetSelectedConfigRequest(uint32_t token,
                                                    ConfigType type);

// Builds the TLV body (NOT including the service_header - QCCI's raw send
// API takes msg_id out of band) for a List Configs request.
std::vector<uint8_t> BuildListConfigsRequest(uint32_t token,
                                              ConfigType type);

// Builds the TLV body for a Get Config Info request.
std::vector<uint8_t> BuildGetConfigInfoRequest(uint32_t token,
                                                ConfigType type,
                                                const std::vector<uint8_t>& id);

// Parses the synchronous ack (Operation Result only) any PDC request
// returns immediately. Returns false if the TLV stream is malformed or
// the mandatory Operation Result TLV is missing - never returns "ok"
// without having actually found and decoded it. Callers MUST treat a
// `false` return as a distinct failure from "decoded, error_status != 0"
// - do not fall through to waiting for an indication in either case
// without checking which one happened.
bool ParseOperationResultOnly(const uint8_t* buf, size_t len,
                               uint16_t* error_status, uint16_t* error_code);

// Three-way outcome classification for a decoded indication, separating
// "the bytes didn't decode safely" (kMalformed - untrusted/corrupt/wrong
// token) from "decoded fine, but the modem itself reported an error"
// (kRejected - result.error_status != 0) from real success. A ListConfigsResult/
// GetConfigInfoResult with `.ok == true` is NOT by itself success - the
// wire could still carry a non-zero Indication Result TLV, which these
// helpers are what actually check.
enum class IndicationOutcome { kSuccess, kRejected, kMalformed };

IndicationOutcome ClassifyListConfigsResult(const ListConfigsResult& result);
IndicationOutcome ClassifyGetConfigInfoResult(
    const GetConfigInfoResult& result);
IndicationOutcome ClassifyGetSelectedConfigResult(
    const GetSelectedConfigResult& result);

// Parses a Get Selected Config indication body, same token-matching
// contract as the other Parse* functions below. Active Id / Pending Id
// are each independently optional on the wire (either, both, or neither
// may be present) - absence means NONE for that field specifically, not
// a parse failure.
GetSelectedConfigResult ParseGetSelectedConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token);

// Parses a List Configs indication body. `expected_token` must match the
// indication's own Token TLV or the indication is rejected (stale/foreign
// response protection) unless `expected_token` is std::nullopt (accept
// any - used only for exploratory/manual probing, not the default CLI
// path).
ListConfigsResult ParseListConfigsIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token);

// Parses a Get Config Info indication body, same token-matching contract.
GetConfigInfoResult ParseGetConfigInfoIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token);

// Extracts just the Token TLV from an arbitrary PDC indication body,
// without assuming which message this is - used by the transport layer to
// route an incoming indication to the request that is actually waiting for
// it before handing off to the message-specific parser above.
std::optional<uint32_t> PeekIndicationToken(const uint8_t* buf, size_t len);

}  // namespace guacamole_pdc
