// guacamole_efs_probe - fixed EFS2 target whitelist. This is the ONLY
// place in this codebase allowed to know a real EFS path string. Every
// other layer (CLI, future orchestration on top of IEfsClient) must go
// through `ResolveTargetName()`/`TargetPath()` and can never construct
// or accept an arbitrary caller-supplied path.
//
// Added per the 2026-09-23 review: efs_wire.h's Build* functions accept
// any std::string path (by design - they are a generic codec, not a
// policy layer), and before Phase D0/D1 wires ANY of them to a real
// transport call, a whitelist enforcement point must sit between the
// CLI and the codec so a future refactor of main.cpp can't accidentally
// pass an arbitrary string through to a real device. Today (Phase D0)
// `main.cpp` doesn't call the codec at all, so this was not yet an
// active risk - this file exists so it never becomes one once
// `SendSubsysCommand()` is implemented in a later phase.
//
// The 13 targets below are exactly the fixed set authorized across
// this phase and its two whitelist expansions - no others, and no CLI
// path argument of any kind.
//
// CONFIRMATION STATUS (updated 2026-09-23 after the third real
// snapshot-mcfg-readonly attempt's full directory enumeration of
// /nv/item_files/mcfg, 34 real entries): kMcfgDir, kMcfgSelDb,
// kMcfgAutoselectByUim are H41_DEVICE_CONFIRMED as real, existing
// entries (their CONTENT has still not been read on a real device -
// that is exactly what the next authorized snapshot run is for). The
// 5 targets added in this expansion
// (kMcfgAutoselectByUimSubscription01, kMcfgSetting, kMcfgSetting1,
// kMcfgSettingSubscription01, kMcfgSettingSubscription01_1) are
// likewise H41_DIRECTORY_ENTRY_CONFIRMED - their exact names came
// directly from that same real READDIR enumeration, not a guess - but
// their CONTENT is equally CONTENT_NOT_YET_READ. kMcfgRfsSwDigestVersion,
// kMcfgCards, kMcfgDebugLog, kMcfgDebugOldLog, kEnableMcfgEfsLogging
// remain this project's original best-guess paths (per
// guacamole_efsd_design_report.md / PHASE_C0_FINDINGS.md), not yet
// independently confirmed as real directory entries by name.

#pragma once

#include <optional>
#include <string>

namespace guacamole_efs {

enum class EfsTarget {
  kMcfgDir,
  kMcfgSelDb,
  kMcfgAutoselectByUim,
  kMcfgRfsSwDigestVersion,
  kMcfgCards,
  kMcfgDebugLog,
  kMcfgDebugOldLog,
  kEnableMcfgEfsLogging,
  // Added 2026-09-23 (second whitelist expansion): these 5 exact names
  // came directly from the third real snapshot-mcfg-readonly attempt's
  // full READDIR enumeration of /nv/item_files/mcfg -
  // H41_DIRECTORY_ENTRY_CONFIRMED, not guessed paths. See this file's
  // header comment.
  kMcfgAutoselectByUimSubscription01,
  kMcfgSetting,
  kMcfgSetting1,
  kMcfgSettingSubscription01,
  kMcfgSettingSubscription01_1,
};

// Canonical, fixed EFS path for each target - see this file's header
// comment for which targets are H41_DEVICE_CONFIRMED /
// H41_DIRECTORY_ENTRY_CONFIRMED vs. still this project's best guess.
const std::string& TargetPath(EfsTarget target);

// Maps a CLI-supplied target NAME (e.g. "sel-db") to a fixed
// EfsTarget. Returns std::nullopt for anything not on this fixed
// list - THIS is the whitelist enforcement point: there is no
// path from an arbitrary input string to a callable EFS2 request
// anywhere in this codebase other than through a value this function
// returns.
std::optional<EfsTarget> ResolveTargetName(const std::string& name);

}  // namespace guacamole_efs
