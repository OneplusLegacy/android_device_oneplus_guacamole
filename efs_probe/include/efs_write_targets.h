// guacamole_efs_probe - fixed EFS2 WRITE target whitelist. Cycle 2,
// Section G. Deliberately a SEPARATE enum/gate from efs_targets.h's
// read whitelist - explicit review requirement ("write target enum
// 별도 whitelist"): a future refactor that accidentally widened the
// READ whitelist must never, by that same mistake, widen what can be
// WRITTEN. There is no conversion between an EfsTarget and an
// EfsWriteTarget anywhere in this codebase, and no function accepts a
// raw path string from a caller for a write - exactly the same
// enforcement shape as efs_targets.h.
//
// Cycle 2's own explicit scope: EXACTLY ONE write target -
// mcfg_sel_db.xml - "first live mutation 대상은 mcfg_sel_db.xml 단
// 하나" (see the review that introduced this file). Adding a second
// write target is a decision for whoever authorizes it next, not
// something this file pre-emptively widens.

#pragma once

#include <optional>
#include <string>

#include "efs_targets.h"  // EfsTarget - see CorrespondingReadTarget() below.

namespace guacamole_efs {

enum class EfsWriteTarget {
  kMcfgSelDb,
};

// Canonical, fixed EFS path for a write target - identical string to
// efs_targets.h's TargetPath(EfsTarget::kMcfgSelDb)
// ("/nv/item_files/mcfg/mcfg_sel_db.xml", H41_DEVICE_CONFIRMED to
// exist - see the real snapshot-mcfg-readonly baseline), but resolved
// through this file's OWN switch statement, not by calling into
// efs_targets.h - keeps the read and write whitelists structurally
// independent even though today they happen to name the same file.
const std::string& TargetPath(EfsWriteTarget target);

// Maps a CLI-supplied target NAME to a fixed EfsWriteTarget. Returns
// std::nullopt for anything not on this fixed list - the whitelist
// enforcement point for writes, mirroring efs_targets.h's
// ResolveTargetName() exactly. NOT used anywhere on the actual
// mutation path today (ApplyPatchPlan()/WriteWholeFile() always take
// an already-resolved EfsWriteTarget, never a name to resolve) - this
// exists purely as a CLI-name<->enum mapping helper for a future
// write-capable CLI command, matching the read side's own
// ResolveTargetName() pattern. It cannot itself be a source of
// arbitrary-path risk: it maps a name to an ENUM VALUE, never to a
// path string.
std::optional<EfsWriteTarget> ResolveWriteTargetName(const std::string& name);

// Explicit mapping from a write target to the READ-side EfsTarget that
// names the SAME real file - added 2026-09-24 (Gate W1 preflight) so
// ApplyPatchPlan() can refuse to apply a PatchPlan built against one
// target to a DIFFERENT write target, even though today there is only
// one member on each side. Deliberately a real function (not just "the
// two enums happen to have matching names") - if a second write target
// is ever added, this mapping must be updated explicitly and
// deliberately, not left to coincidental enum ordering.
EfsTarget CorrespondingReadTarget(EfsWriteTarget target);

}  // namespace guacamole_efs
