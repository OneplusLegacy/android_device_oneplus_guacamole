// guacamole_efs_probe - the integrated `snapshot-mcfg-readonly`
// orchestrator: runs the fixed 14-step sequence (expanded from 9 on
// 2026-09-23, second whitelist expansion, once the third real
// snapshot-mcfg-readonly attempt's real READDIR enumeration confirmed
// 5 additional dual-SIM/state file names on /nv/item_files/mcfg - see
// efs_targets.h) over efs_targets.h's whitelist in one pass, so a
// single real-device invocation gathers everything needed for
// post-mortem analysis instead of requiring a separate authorized run
// per target (see the completion report that introduced this file for
// why).
//
// PRESENCE VS. FAILURE, read this before changing anything (CORRECTED
// 2026-09-23 preflight review - the previous rule here was wrong and
// is kept below, struck through in spirit, purely as a record of what
// changed and why): a STRUCTURALLY VALID STAT response (`.ok == true`)
// carrying a nonzero `diag_errno` NO LONGER means "target absent" for
// ANY target, mandatory or optional. qtools' own efs_stat() source
// itself does not distinguish "file not found" from "some other EFS
// error" via a specific errno value, and this project has never had a
// confirmed "no such entry" constant (unlike PDC's cited
// QMI_PROTOCOL_ERROR_NO_ENTRY=50) - claiming `kAbsent` on nothing more
// than "STAT returned SOME error" risked misclassifying a real
// permission/I-O/protocol error as "this file doesn't exist". The
// corrected rule:
//   - A nonzero STAT diag_errno on an OPTIONAL target sets
//     `TargetPresence::kUnknown` (NOT kAbsent), records the errno in
//     `message`, and the snapshot CONTINUES to the next step - this is
//     now reported as a WARNING (`SnapshotOutcome::kDoneWithWarnings`),
//     not a silent success and not a full failure.
//   - A nonzero STAT diag_errno on the MANDATORY `/nv/item_files/mcfg`
//     target still aborts the entire snapshot (STAT succeeding on the
//     root directory is a real precondition for every later step), but
//     `fail_reason` now says "mandatory STAT EFS error, diag_errno=N",
//     never "mandatory target is absent" - we still don't actually
//     know it's absent.
//   - `TargetPresence::kAbsent` is RESERVED for a future cycle once a
//     specific "no such entry" errno is confirmed on the real device -
//     nothing in this file sets it today.
// ANYTHING ELSE - a malformed/short response (`.ok == false`), a
// transport failure, a timeout, a wrong-header correlation failure, an
// exceeded safety cap - is still fail-fast and still stops the ENTIRE
// snapshot immediately, for both mandatory and optional steps alike
// (see `RunMcfgReadonlySnapshot`'s own implementation) - that part is
// unchanged.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "efs_client.h"
#include "efs_dir_lister.h"
#include "efs_targets.h"

namespace guacamole_efs {

enum class TargetPresence { kUnknown, kExists, kAbsent };

struct EfsSnapshotEntry {
  std::string step_name;  // matches the CLI's own "=== STEP: <name> ===" header.
  EfsTarget target = EfsTarget::kMcfgDir;
  // Output metadata only. Set exclusively by the internal muxd collector
  // from this invocation's successful fixed-parent directory enumeration.
  std::string discovered_path;
  std::string discovered_name;
  int32_t listed_size = 0;
  std::string collection_status;  // VERIFIED / SKIPPED / FAILED for muxd

  bool is_mandatory = false;
  bool is_directory = false;
  TargetPresence presence = TargetPresence::kUnknown;

  // STAT fields - valid only if has_stat is true. Populated for BOTH
  // file and directory targets (a directory step STATs its own target
  // too, when check_presence_first applies - see efs_snapshot.cpp).
  bool has_stat = false;
  int32_t stat_size = 0;
  int32_t stat_mode = 0;
  int32_t stat_nlink = 0;
  int32_t stat_atime = 0;
  int32_t stat_mtime = 0;
  int32_t stat_ctime = 0;

  // File-target fields (is_directory == false), valid only if the
  // corresponding has_* flag is true.
  bool has_data = false;
  std::vector<uint8_t> data;
  bool has_sha256 = false;
  std::array<uint8_t, 32> sha256{};

  // Directory-target fields (is_directory == true).
  bool has_dir_entries = false;
  std::vector<EfsDirEntry> dir_entries;

  // Cross-check against STEP 2's own `/nv/item_files/mcfg` listing -
  // valid only for the 4 targets that are direct children of that
  // directory (sel_db.xml, autoselect_by_uim, rfs_sw_digest_version,
  // cards - NOT the 3 targets outside it: the 2 debug logs and the
  // efslogging flag, which this field is left false for). Set
  // independently of `presence`/`message` - a STAT error (presence
  // kUnknown) alongside `directory_entry_seen == true` is a genuine,
  // preserved CONTRADICTION (the directory says this name exists, STAT
  // on it failed) that Cycle 2 needs intact, not resolved one way or
  // the other by this code guessing which signal to trust.
  bool directory_entry_seen = false;

  std::string message;  // diagnostic detail - a failure/warning reason, etc.
};

enum class SnapshotOutcome { kDone, kDoneWithWarnings, kFailed };

struct EfsSnapshotResult {
  SnapshotOutcome outcome = SnapshotOutcome::kFailed;
  std::string fail_reason;  // populated only when outcome == kFailed.
  std::vector<EfsSnapshotEntry> entries;  // in STEP order, one per step
                                            // actually attempted - a
                                            // fail-fast stop means later
                                            // steps are simply absent
                                            // from this list, not
                                            // present-but-empty.
};

// Called exactly once at the START of each step (before any IEfsClient
// call for that step), with the step's own name - lets a caller (e.g.
// the CLI) print a live "=== STEP: <name> ===" header and correlate an
// EfsRealClient's CallLog() entries to the step that produced them
// (see main.cpp's `snapshot-mcfg-readonly` command), without this
// function needing to know anything about wire-level logging itself.
// Entirely optional - every existing/future caller not needing this
// simply omits it.
using EfsSnapshotStepHook = std::function<void(const std::string& step_name)>;

// Runs the fixed 14-step sequence: STAT the mandatory `/nv/item_files/mcfg`
// directory; list it; STAT+read `mcfg_sel_db.xml`,
// `mcfg_autoselect_by_uim`, `mcfg_autoselect_by_uim_Subscription01`,
// `mcfg_setting`, `mcfg_setting_1`, `mcfg_setting_Subscription01`,
// `mcfg_setting_Subscription01_1`, `mcfg_rfs_sw_digest_version`; list
// the optional `/nv/item_files/mcfg/cards` directory; STAT+read
// `/mcfg_debug.log`, `/mcfg_debug_old.log`, `/enable_mcfg_efslogging`.
// Then read validated muxd entries discovered in step 2 (no caller path API).
// Existing fixed-target steps/order are unchanged. No retries anywhere.
EfsSnapshotResult RunMcfgReadonlySnapshot(
    IEfsClient* client, EfsSnapshotStepHook on_step_start = nullptr);

}  // namespace guacamole_efs
