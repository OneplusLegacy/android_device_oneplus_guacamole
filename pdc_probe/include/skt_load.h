// guacamole_pdc_probe - Phase C0: SKT-only PDC Load Config state machine.
//
// THIS IS THE ONLY PLACE IN THE CODEBASE THAT WRITES ANYTHING TO THE
// MODEM. Everything it does is scoped exactly to the Phase C0 spec:
//   - Loads exactly one config (the internally-known-good stock SKT
//     MBN - see known_profiles.h, no external path input possible).
//   - Never calls Set Selected Config / Activate / Deactivate / Delete.
//   - Never touches EFS2, mcfg_sel_db.xml, mcfg_autoselect_by_uim,
//     mcfg_rfs_sw_digest_version, or /nv/item_files/mcfg/cards.
//   - Never restarts the modem or reboots the device.
//   - Never deletes an existing config, including on its own "failure".
//   - Never retries a timed-out chunk - a timeout moves the state
//     machine to kUnknownResult, which re-queries (List Configs for
//     diagnostics, Get Config Info for the authoritative answer) to
//     determine the real outcome instead of guessing or resending.
//
// The state sequence (see LoadState below) matches the Phase C0 spec's
// required states exactly: PRECHECK, DISCOVER, OPEN_FILE, LOAD_BEGIN,
// LOAD_CHUNKS, WAIT_COMPLETION, VERIFY_LIST, VERIFY_INFO, DONE,
// UNKNOWN_RESULT, FAILED. Post-hardening (see PHASE_C0_FINDINGS.md's
// "real-device test #2"): VERIFY_LIST is diagnostic-only (List Configs
// is not a reliable enumeration - a real device test found it can
// silently omit an entry Get Config Info still resolves); VERIFY_INFO
// (Get Config Info on the exact config ID) is the sole authoritative
// presence/success check, both pre-load (DISCOVER) and post-load.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "known_profiles.h"
#include "pdc_client.h"

namespace guacamole_pdc {

enum class LoadState {
  kPrecheck,
  kDiscover,
  kOpenFile,
  kLoadBegin,
  kLoadChunks,
  kWaitCompletion,
  kVerifyList,
  kVerifyInfo,
  kDone,
  kUnknownResult,
  kFailed,
};

const char* LoadStateToString(LoadState state);

// Tri-state result for the before/after selected-config comparison.
// Deliberately NOT a bool: "we never managed to query it" must be
// distinguishable from "we queried it and it matched" - collapsing
// those two into the same false-y value was a real bug found on the
// first real-device run (see PHASE_C0_FINDINGS.md's real-device-test
// addendum): a failed chunk transfer aborted before ever re-querying
// `selected`, but the report printed `selected_active_after=NONE` (as
// if the modem had actively confirmed no active config) instead of
// making the "we didn't check" case visually distinct.
enum class SelectionCheckResult { kUnchanged, kChanged, kUnknown };

const char* SelectionCheckResultToString(SelectionCheckResult result);

// One snapshot of Get Selected Config's result, explicit about whether
// the query even happened. `active_id`/`pending_id` are only meaningful
// when `queried == true`; even then, either may still be absent (the
// modem legitimately reporting NONE), which is different again.
struct SelectedSnapshot {
  bool queried = false;
  std::optional<std::vector<uint8_t>> active_id;
  std::optional<std::vector<uint8_t>> pending_id;
};

struct SktLoadReport {
  LoadState final_state = LoadState::kFailed;
  std::string message;  // human-readable explanation of final_state

  // File-side facts, filled in as PRECHECK/OPEN_FILE succeed.
  uint32_t file_size = 0;
  std::vector<uint8_t> config_id;  // SHA-1 of the file - both the value
                                    // we send on the wire AND the value
                                    // to look for afterward. Not a
                                    // hardcoded expectation - computed
                                    // fresh from the actual file bytes
                                    // every run.

  // Transfer progress.
  uint32_t chunks_sent = 0;
  uint32_t bytes_sent = 0;

  // Before/after "selected" safety check (the second of the two big
  // unresolved questions this whole project has been chasing - see
  // guacamole_efsd_stock_modem_design.md item 12 and its Phase B2/C0
  // follow-up).
  SelectedSnapshot selected_before;
  SelectedSnapshot selected_after;
  SelectionCheckResult selection_check = SelectionCheckResult::kUnknown;

  // AUTHORITATIVE presence, from Get Config Info on the exact config ID
  // - never from List Configs (see ConfigPresence's comment in
  // pdc_client.h: a real device test found List Configs can silently
  // omit an entry that Get Config Info still resolves successfully).
  ConfigPresence config_presence_before = ConfigPresence::kUnknown;
  ConfigPresence config_presence_after = ConfigPresence::kUnknown;
  GetConfigInfoOutcome info_before;  // the actual GetConfigInfo call
                                      // behind config_presence_before.
  GetConfigInfoOutcome info_after;   // the actual GetConfigInfo call
                                      // behind config_presence_after.
  bool already_existed_before_load = false;  // == (config_presence_before
                                              // == kExists). Load was
                                              // skipped entirely.

  // List Configs' result is DIAGNOSTIC/ADVISORY ONLY from here on - see
  // ConfigPresence's comment. `count` in `list` command output, and
  // `found_in_list_after` here, are "how many/whether entries this
  // particular List Configs call happened to return" - NOT proof of the
  // modem's true total config count or of a specific ID's non-existence.
  // Never gates any decision in this state machine; kept purely so the
  // report still shows what a manual `list` would have shown at each
  // point. Populated even on a FAILED outcome where possible (read-only
  // re-checks after a mid-transfer failure do not overwrite the failure
  // itself - see RunPostMortemVerification in skt_load.cpp).
  bool found_in_list_after = false;
};

// Runs the full Phase C0 state machine against the internally-known SKT
// profile (known_profiles.h): reads the real stock file off the
// filesystem, then calls RunLoadFromBytes below. `client` must already
// be Init()'d. This is what production code (main.cpp) calls.
SktLoadReport RunLoadSkt(IPdcClient& client, uint32_t timeout_ms);

// Cycle 2 Section H: the SAME state machine, generalized to any of the
// 3 Korean carriers via `known_profiles.h`'s `Carrier` enum - never a
// raw path, never a raw config ID. Purely additive: `RunLoadSkt()`
// above is unchanged and still the exact entry point Phase C0's real
// device test used; this is a thin dispatcher on top of the SAME
// already-proven `RunLoadFromBytes()` state machine, reading the
// right file via `GetProfileForCarrier(carrier)` instead of a
// hardcoded `GetSktProfile()`. KT/LGU real-device loads remain
// NOT YET AUTHORIZED (Gate W1) - only host/fake-client tests exercise
// this for KT/LGU today; SKT via this same function has the identical
// real-device-proven behavior as `RunLoadSkt()`.
SktLoadReport RunLoadCarrier(IPdcClient& client, Carrier carrier,
                              uint32_t timeout_ms);

// The actual state machine (DISCOVER through DONE/FAILED/UNKNOWN_RESULT),
// taking already-read file bytes and a profile to validate them against,
// instead of doing its own filesystem I/O. Split out specifically so
// tests/skt_load_test.cpp can exercise the full chunking/verification/
// safety-check logic against synthetic in-memory buffers and a fake
// IPdcClient, without a real device or real MBN bytes (which the Phase
// C0 spec explicitly says must not be embedded in host tests). Takes
// the abstract interface for the same reason. Production's RunLoadSkt
// above is a thin wrapper: real file I/O against known_profiles.h, then
// this function.
SktLoadReport RunLoadFromBytes(IPdcClient& client,
                                const std::vector<uint8_t>& file_bytes,
                                const KnownProfile& profile,
                                uint32_t timeout_ms);

}  // namespace guacamole_pdc
