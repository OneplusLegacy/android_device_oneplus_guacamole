// guacamole_pdc_probe - PDC Set Selected Config wire format. Cycle 2
// Section K wire codec, now used by Round 3's separate debug-only controlled
// SKT/Common experiment. No generic mutation CLI or production Binder caller.
// Creating this tool does not authorize a real SetSelected experiment.
//
// Message ID and TLV layout: data/qmi-service-pdc.json (same public,
// cross-device-validated libqmi source used for the rest of this
// project's PDC wire format - see pdc_wire.h for the full provenance
// story), fetched fresh 2026-09-24 and cross-checked against this
// codebase's OWN already-proven constants:
//   - Message id 0x0023, immediately adjacent to the already-confirmed
//     kMsgGetSelectedConfig=0x0022/kMsgListConfigs=0x0024 - consistent
//     with, but not derived by extrapolating, that numbering; the exact
//     value was independently fetched from the source JSON, not guessed
//     from the pattern of neighboring ids.
//   - Request: the "Config Type And Id" common-ref (TLV 0x01) + "Token"
//     common-ref (TLV 0x10) - the JSON's own "Common" section defines
//     both ids as 0x01/0x10 respectively, EXACTLY matching this
//     codebase's pre-existing kTlvConfigTypeAndId/kTlvToken constants
//     (pdc_wire.h) already proven correct for Get Config Info's
//     identically-shaped request - so this request is built with the
//     EXACT SAME byte-for-byte encoding as BuildGetConfigInfoRequest(),
//     just under a different message id.
//   - Synchronous response: Operation Result + Token only (same shape
//     as every other PDC request in this codebase) - ParseOperationResultOnly()
//     is reused verbatim, no new sync-ack parser needed.
//   - Indication: Indication Result (TLV 0x01) + Token (TLV 0x10) ONLY -
//     no active/pending id, no config-specific payload of any kind.
//     This is the SIMPLEST indication shape in the whole PDC API (even
//     simpler than Load Config's, which carries Received/RemainingSize/
//     FrameReset) - success or failure and nothing else. This directly
//     answers part of item K's research question ("subscription selector
//     semantics, config type, pending/active semantics, indication
//     semantics") for the indication side: THIS call reports only
//     whether the modem accepted the selection, never which config
//     ended up active/pending - confirming that fact requires a
//     SEPARATE Get Selected Config call after the fact. No such device
//     experiment is exposed by this round.
//
// SUBSCRIPTION SELECTOR: the request carries ONLY {Config Type, Config
// Id} via "Config Type And Id" - there is no separate SIM-slot/
// subscription-index field anywhere in this message's TLVs. This
// matches the same shape Get Selected Config's OWN request already
// uses (see pdc_wire.h's BuildGetSelectedConfigRequest, which also
// takes no slot parameter) - PDC's "Config Type" (kPlatform/kSoftware)
// is evidently the only selector dimension this service exposes, at
// least for THIS message. Any real multi-SIM-slot semantics (if they
// exist at all for this device) must live at a DIFFERENT layer (e.g.
// QMI_UIM or a subscription-manager-level API), never guessed here.
//
// PENDING VS ACTIVE SEMANTICS: NOT resolvable from this message's wire
// shape alone (see indication note above) - Set Selected Config's own
// indication does not distinguish "now active" from "now pending
// pending a reboot/other event". Get Selected Config's ACTIVE ID vs
// PENDING ID TLVs (already implemented, pdc_wire.h) are the only way to
// observe that distinction, and only by a separately authorized experiment.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "pdc_wire.h"  // ConfigType, IndicationOutcome, kTlv* shared constants

namespace guacamole_pdc {

constexpr uint32_t kMsgSetSelectedConfig = 0x0023;

struct SetSelectedConfigResult {
  bool ok = false;
  uint16_t error_status = 0;
  uint16_t error_code = 0;
};

// Builds a Set Selected Config request. Byte-for-byte the SAME "Config
// Type And Id" encoding as BuildGetConfigInfoRequest() (see this file's
// header comment) - deliberately NOT re-implemented independently, to
// avoid two encodings of the same common-ref TLV silently drifting
// apart.
std::vector<uint8_t> BuildSetSelectedConfigRequest(uint32_t token,
                                                     ConfigType type,
                                                     const std::vector<uint8_t>& id);

// Parses a Set Selected Config indication body - same token-matching
// contract as every other Parse* function in this project. There is no
// payload beyond Indication Result/Token (see file header), so this
// function's only job is to confirm the indication decoded safely and
// extract the result code - directly analogous to
// ParseOperationResultOnly() but for the INDICATION stage, not the
// synchronous ack.
SetSelectedConfigResult ParseSetSelectedConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token);

IndicationOutcome ClassifySetSelectedConfigResult(
    const SetSelectedConfigResult& result);

}  // namespace guacamole_pdc
