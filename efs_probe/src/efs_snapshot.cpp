#include "efs_snapshot.h"

#include "efs_reader.h"

#include <algorithm>
#include <set>
#include "hash.h"

namespace guacamole_efs {

namespace {

// Dynamic read-only exception: this resolver/reader are translation-unit
// private. Only RunMcfgReadonlySnapshot supplies entries, directly from its
// completed ListDirectory(fixed kMcfgDir). No caller-supplied path API exists.
constexpr int32_t kMuxdMaxBytes = 4096;
constexpr size_t kMuxdMaxFiles = 256;
constexpr char kMuxdPrefix[] = "mcfg_sw_muxd_version";

bool IsMuxdBasename(const std::string& name) {
  const std::string base(kMuxdPrefix);
  if (name == base) return true;
  if (name.compare(0, base.size() + 1, base + "_") != 0) return false;
  std::string suffix = name.substr(base.size() + 1);
  const std::string subscription = "Subscription01";
  if (suffix == subscription) return true;
  if (suffix.compare(0, subscription.size() + 1, subscription + "_") == 0)
    suffix.erase(0, subscription.size() + 1);
  // Index digits are opaque names, never interpreted as carrier mappings.
  return !suffix.empty() && suffix.size() <= 10 &&
      std::all_of(suffix.begin(), suffix.end(), [](char c) {
        return c >= '0' && c <= '9';
      });
}

bool IsMuxdFileType(int32_t entry_type, int32_t mode) {
  const int32_t kind = mode & 0170000;
  // H.41 baseline READDIR: muxd entries have type=15, mode=0xe1ff
  // (item mode kind 0160000). This is explicitly allowed, not treated as
  // a POSIX regular file. Reject directories, symlinks and unknown kinds.
  return (entry_type == 0 && kind == 0100000) ||
         (entry_type == 15 && kind == 0160000);
}

EfsSnapshotEntry ReadDiscoveredMuxd(IEfsClient* client, const EfsDirEntry& d,
                                    const EfsSnapshotStepHook& hook) {
  EfsSnapshotEntry e;
  e.step_name = "STAT+READ discovered muxd";
  if (hook) hook(e.step_name);
  e.directory_entry_seen = true;
  e.collection_status = "SKIPPED";
  e.listed_size = d.size;
  if (!IsMuxdBasename(d.name)) {
    e.is_directory = true;  // no fabricated file target for an unsafe name
    e.message = "muxd: rejected basename; see original directory listing";
    return e;
  }
  e.discovered_name = d.name;
  e.discovered_path = TargetPath(EfsTarget::kMcfgDir) + "/" + d.name;
  if (!IsMuxdFileType(d.entry_type, d.mode)) {
    e.message = "muxd: skipped directory/symlink/unsupported entry type or mode";
    return e;
  }
  e.collection_status = "FAILED";
  if (d.size < 0 || d.size > kMuxdMaxBytes) {
    e.message = "muxd: listing size outside 0..4096 byte cap";
    return e;
  }
  const auto st = client->Stat(e.discovered_path);
  if (!st.ok || st.diag_errno != 0 || st.size != d.size ||
      !IsMuxdFileType(d.entry_type, st.mode)) {
    e.message = "muxd: STAT failed or listing/type/size changed; UNKNOWN";
    return e;
  }
  e.has_stat = true;
  e.stat_size = st.size;
  e.stat_mode = st.mode;
  e.stat_nlink = st.nlink;
  e.stat_atime = st.atime;
  e.stat_mtime = st.mtime;
  e.stat_ctime = st.ctime;
  const auto opened = client->OpenReadOnly(e.discovered_path);
  if (!opened.ok || opened.diag_errno != 0 || opened.fd < 0) {
    e.message = "muxd: O_RDONLY open failed; UNKNOWN";
    return e;
  }
  std::vector<uint8_t> data;
  uint32_t offset = 0;
  while (offset < static_cast<uint32_t>(st.size)) {
    const uint32_t count = std::min(kMaxReadRequestBytes,
        static_cast<uint32_t>(st.size) - offset);
    const auto r = client->Read(opened.fd, count, offset);
    if (!r.ok || r.diag_errno != 0 || r.fd != opened.fd ||
        r.offset != offset || r.bytes_read != static_cast<int32_t>(count) ||
        r.data.size() != count) {
      client->Close(opened.fd);  // cleanup only; poisoned transport sends nothing
      e.message = "muxd: read failed/partial/mismatched; UNKNOWN; no retry";
      return e;
    }
    data.insert(data.end(), r.data.begin(), r.data.end());
    offset += count;
  }
  const auto closed = client->Close(opened.fd);
  if (!closed.ok || closed.errno_value != 0) {
    e.message = "muxd: close failed; UNKNOWN";
    return e;
  }
  const auto after = client->Stat(e.discovered_path);
  if (!after.ok || after.diag_errno != 0 || after.size != st.size ||
      after.mode != st.mode || after.mtime != st.mtime || after.ctime != st.ctime) {
    e.message = "muxd: post-read STAT failed/changed; UNKNOWN";
    return e;
  }
  e.data = std::move(data);
  e.has_data = true;
  e.sha256 = Sha256(e.data.data(), e.data.size());
  e.has_sha256 = true;
  e.presence = TargetPresence::kExists;
  e.collection_status = "VERIFIED";
  return e;
}

// Extract a fixed target basename for the step-2 listing cross-check.
std::string BaseName(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

EfsSnapshotEntry RunFileStep(IEfsClient* client, const std::string& step_name,
                              EfsTarget target, bool mandatory,
                              const EfsSnapshotStepHook& on_step_start,
                              bool* abort, std::string* abort_reason,
                              bool* warning) {
  if (on_step_start) on_step_start(step_name);

  EfsSnapshotEntry entry;
  entry.step_name = step_name;
  entry.target = target;
  entry.is_mandatory = mandatory;

  const std::string& path = TargetPath(target);
  EfsStatResult st = client->Stat(path);
  if (!st.ok) {
    entry.message = "stat: malformed/short response";
    *abort = true;
    *abort_reason = step_name + ": " + entry.message;
    return entry;
  }
  if (st.diag_errno != 0) {
    // CORRECTED 2026-09-23: a nonzero STAT errno does NOT prove
    // absence - see efs_snapshot.h's header comment. Optional targets
    // continue as a WARNING; the mandatory case is handled by the
    // caller (only STEP 1 is ever mandatory=true here, and it never
    // reaches this function - see RunMcfgReadonlySnapshot's inline
    // STEP 1 block), so `mandatory` is always false in THIS function
    // today, but the abort path is kept for defensive symmetry with
    // RunDirStep's identical logic.
    entry.presence = TargetPresence::kUnknown;
    entry.message = "stat: EFS error, diag_errno=" +
                     std::to_string(st.diag_errno) + " (absence not proven)";
    if (mandatory) {
      *abort = true;
      *abort_reason = step_name + ": mandatory STAT EFS error, diag_errno=" +
                       std::to_string(st.diag_errno);
      return entry;
    }
    *warning = true;
    return entry;
  }

  entry.presence = TargetPresence::kExists;
  entry.has_stat = true;
  entry.stat_size = st.size;
  entry.stat_mode = st.mode;
  entry.stat_nlink = st.nlink;
  entry.stat_atime = st.atime;
  entry.stat_mtime = st.mtime;
  entry.stat_ctime = st.ctime;

  EfsReadFileResult rf = ReadWholeFile(client, path);
  if (rf.state != EfsReadFileState::kDone) {
    entry.message = "read: " + rf.message;
    *abort = true;
    *abort_reason = step_name + ": " + entry.message;
    return entry;
  }
  entry.has_data = true;
  entry.data = rf.data;
  entry.sha256 = Sha256(entry.data.data(), entry.data.size());
  entry.has_sha256 = true;
  return entry;
}

EfsSnapshotEntry RunDirStep(IEfsClient* client, const std::string& step_name,
                             EfsTarget target, bool mandatory,
                             bool check_presence_first,
                             const EfsSnapshotStepHook& on_step_start,
                             bool* abort, std::string* abort_reason,
                             bool* warning) {
  if (on_step_start) on_step_start(step_name);

  EfsSnapshotEntry entry;
  entry.step_name = step_name;
  entry.target = target;
  entry.is_mandatory = mandatory;
  entry.is_directory = true;

  const std::string& path = TargetPath(target);

  if (check_presence_first) {
    EfsStatResult st = client->Stat(path);
    if (!st.ok) {
      entry.message = "stat: malformed/short response";
      *abort = true;
      *abort_reason = step_name + ": " + entry.message;
      return entry;
    }
    if (st.diag_errno != 0) {
      // See RunFileStep's identical comment - CORRECTED 2026-09-23.
      entry.presence = TargetPresence::kUnknown;
      entry.message = "stat: EFS error, diag_errno=" +
                       std::to_string(st.diag_errno) +
                       " (absence not proven)";
      if (mandatory) {
        *abort = true;
        *abort_reason = step_name +
                         ": mandatory STAT EFS error, diag_errno=" +
                         std::to_string(st.diag_errno);
        return entry;
      }
      *warning = true;
      return entry;
    }
    entry.presence = TargetPresence::kExists;
    entry.has_stat = true;
    entry.stat_size = st.size;
    entry.stat_mode = st.mode;
    entry.stat_nlink = st.nlink;
    entry.stat_atime = st.atime;
    entry.stat_mtime = st.mtime;
    entry.stat_ctime = st.ctime;
  } else {
    // Presence already confirmed by an earlier mandatory step (STEP 1
    // for STEP 2's /nv/item_files/mcfg listing) - no separate check.
    entry.presence = TargetPresence::kExists;
  }

  ListDirectoryResult lr = ListDirectory(client, path);
  if (lr.state != ListDirectoryState::kDone) {
    entry.message = "listdir: " + lr.message;
    *abort = true;
    *abort_reason = step_name + ": " + entry.message;
    return entry;
  }
  entry.has_dir_entries = true;
  entry.dir_entries = std::move(lr.entries);
  return entry;
}

// STEP 2 cross-check (2026-09-23 preflight review, item 5): does
// `mcfg_dir_entries` (STEP 2's own listing of /nv/item_files/mcfg)
// contain an entry whose name matches `target`'s basename? Used only
// for the 4 targets that are direct children of that directory -
// callers never call this for the 3 targets outside it (the 2 debug
// logs and the efslogging flag).
bool NameSeenInListing(const std::vector<EfsDirEntry>& mcfg_dir_entries,
                        EfsTarget target) {
  const std::string basename = BaseName(TargetPath(target));
  for (const auto& e : mcfg_dir_entries) {
    if (e.name == basename) return true;
  }
  return false;
}

}  // namespace

EfsSnapshotResult RunMcfgReadonlySnapshot(IEfsClient* client,
                                           EfsSnapshotStepHook on_step_start) {
  EfsSnapshotResult result;
  bool abort = false;
  bool warning = false;
  std::string abort_reason;

  // STEP 1 (mandatory): STAT /nv/item_files/mcfg
  {
    if (on_step_start) on_step_start("STAT /nv/item_files/mcfg");
    EfsSnapshotEntry e;
    e.step_name = "STAT /nv/item_files/mcfg";
    e.target = EfsTarget::kMcfgDir;
    e.is_mandatory = true;
    const std::string& path = TargetPath(EfsTarget::kMcfgDir);
    EfsStatResult st = client->Stat(path);
    if (!st.ok) {
      e.message = "stat: malformed/short response";
      abort = true;
      abort_reason = e.step_name + ": " + e.message;
    } else if (st.diag_errno != 0) {
      // CORRECTED 2026-09-23: no longer claims "absent" - see
      // efs_snapshot.h's header comment. Still aborts - STAT
      // succeeding on the mandatory root directory is a real
      // precondition for every later step - but the wording no longer
      // asserts something this code doesn't actually know.
      e.presence = TargetPresence::kUnknown;
      e.message = "stat: EFS error, diag_errno=" +
                   std::to_string(st.diag_errno) + " (absence not proven)";
      abort = true;
      abort_reason = e.step_name + ": mandatory STAT EFS error, diag_errno=" +
                      std::to_string(st.diag_errno);
    } else {
      e.presence = TargetPresence::kExists;
      e.has_stat = true;
      e.stat_size = st.size;
      e.stat_mode = st.mode;
      e.stat_nlink = st.nlink;
      e.stat_atime = st.atime;
      e.stat_mtime = st.mtime;
      e.stat_ctime = st.ctime;
    }
    result.entries.push_back(e);
  }
  if (abort) {
    result.outcome = SnapshotOutcome::kFailed;
    result.fail_reason = abort_reason;
    return result;
  }

  // STEP 2 (mandatory - directory already confirmed present by STEP 1):
  // OPENDIR/READDIR /nv/item_files/mcfg
  {
    auto e = RunDirStep(client, "OPENDIR/READDIR /nv/item_files/mcfg",
                         EfsTarget::kMcfgDir, /*mandatory=*/true,
                         /*check_presence_first=*/false, on_step_start,
                         &abort, &abort_reason, &warning);
    result.entries.push_back(e);
  }
  if (abort) {
    result.outcome = SnapshotOutcome::kFailed;
    result.fail_reason = abort_reason;
    return result;
  }
  // STEP 2's own listing, for the cross-check below - COPIED, not
  // referenced: `result.entries` is a std::vector that keeps growing
  // via push_back() below, and a reference into one of ITS elements
  // (result.entries[1].dir_entries) would dangle the instant a later
  // push_back() reallocates the vector's buffer - a real bug an
  // earlier revision of this function had (caught by
  // EfsSnapshot.FullSuccessAllPresent under a plain build, no
  // sanitizer needed: mcfg_dir_entries silently read back empty for
  // every step after the first). Empty if STEP 2 somehow produced no
  // entries (an empty mcfg/ directory), which is not itself a failure
  // at this point (fail-fast already returned above if listing failed
  // outright).
  const std::vector<EfsDirEntry> mcfg_dir_entries =
      result.entries[1].dir_entries;

  // STEPS 3-10 (optional file targets, all direct children of
  // /nv/item_files/mcfg - eligible for the STEP 2 cross-check). The 5
  // targets added 2026-09-23 (second whitelist expansion:
  // kMcfgAutoselectByUimSubscription01, kMcfgSetting, kMcfgSetting1,
  // kMcfgSettingSubscription01, kMcfgSettingSubscription01_1) came
  // directly from the third real snapshot-mcfg-readonly attempt's real
  // READDIR enumeration - their names are H41_DIRECTORY_ENTRY_CONFIRMED,
  // not guessed. This step deliberately reads ONLY raw bytes/size/
  // SHA-256 - no interpretation of mcfg_setting*'s content/schema
  // happens anywhere in this codebase; that is explicitly deferred to
  // Cycle 2, which will have real captured bytes to work from instead
  // of guessing.
  struct FileStep {
    const char* name;
    EfsTarget target;
  };
  const FileStep steps_3_to_10[] = {
      {"STAT+READ mcfg_sel_db.xml", EfsTarget::kMcfgSelDb},
      {"STAT+READ mcfg_autoselect_by_uim", EfsTarget::kMcfgAutoselectByUim},
      {"STAT+READ mcfg_autoselect_by_uim_Subscription01",
       EfsTarget::kMcfgAutoselectByUimSubscription01},
      {"STAT+READ mcfg_setting", EfsTarget::kMcfgSetting},
      {"STAT+READ mcfg_setting_1", EfsTarget::kMcfgSetting1},
      {"STAT+READ mcfg_setting_Subscription01",
       EfsTarget::kMcfgSettingSubscription01},
      {"STAT+READ mcfg_setting_Subscription01_1",
       EfsTarget::kMcfgSettingSubscription01_1},
      {"STAT+READ mcfg_rfs_sw_digest_version",
       EfsTarget::kMcfgRfsSwDigestVersion},
  };
  for (const auto& s : steps_3_to_10) {
    auto e = RunFileStep(client, s.name, s.target, /*mandatory=*/false,
                          on_step_start, &abort, &abort_reason, &warning);
    e.directory_entry_seen = NameSeenInListing(mcfg_dir_entries, s.target);
    result.entries.push_back(e);
    if (abort) {
      result.outcome = SnapshotOutcome::kFailed;
      result.fail_reason = abort_reason;
      return result;
    }
  }

  // STEP 11 (optional directory target, also a direct child of
  // /nv/item_files/mcfg): OPENDIR/READDIR /nv/item_files/mcfg/cards
  {
    auto e = RunDirStep(client, "OPENDIR/READDIR /nv/item_files/mcfg/cards",
                         EfsTarget::kMcfgCards, /*mandatory=*/false,
                         /*check_presence_first=*/true, on_step_start,
                         &abort, &abort_reason, &warning);
    e.directory_entry_seen =
        NameSeenInListing(mcfg_dir_entries, EfsTarget::kMcfgCards);
    result.entries.push_back(e);
  }
  if (abort) {
    result.outcome = SnapshotOutcome::kFailed;
    result.fail_reason = abort_reason;
    return result;
  }

  // STEPS 12-14 (optional file targets - NOT children of
  // /nv/item_files/mcfg, so no directory_entry_seen cross-check
  // applies; it stays at its default of false for these three).
  const FileStep steps_12_to_14[] = {
      {"STAT+READ /mcfg_debug.log", EfsTarget::kMcfgDebugLog},
      {"STAT+READ /mcfg_debug_old.log", EfsTarget::kMcfgDebugOldLog},
      {"STAT+READ /enable_mcfg_efslogging", EfsTarget::kEnableMcfgEfsLogging},
  };
  for (const auto& s : steps_12_to_14) {
    auto e = RunFileStep(client, s.name, s.target, /*mandatory=*/false,
                          on_step_start, &abort, &abort_reason, &warning);
    result.entries.push_back(e);
    if (abort) {
      result.outcome = SnapshotOutcome::kFailed;
      result.fail_reason = abort_reason;
      return result;
    }
  }

  // Append muxd results after all original fixed steps, using ONLY the
  // successful step-2 enumeration. Invalid types are explicit warnings;
  // size/read/integrity failures abort. No mutation interfaces are used.
  std::set<std::string> muxd_names;
  for (const auto& d : mcfg_dir_entries) {
    if (d.name.compare(0, sizeof(kMuxdPrefix) - 1, kMuxdPrefix) != 0) continue;
    if (!muxd_names.insert(d.name).second || muxd_names.size() > kMuxdMaxFiles) {
      result.outcome = SnapshotOutcome::kFailed;
      result.fail_reason = "muxd: duplicate entry or 256-file cap exceeded";
      return result;
    }
  }
  for (const auto& d : mcfg_dir_entries) {
    if (d.name.compare(0, sizeof(kMuxdPrefix) - 1, kMuxdPrefix) != 0) continue;
    auto e = ReadDiscoveredMuxd(client, d, on_step_start);
    const bool failed = e.collection_status == "FAILED";
    warning = warning || e.collection_status == "SKIPPED";
    if (failed) result.fail_reason = e.message;
    result.entries.push_back(std::move(e));
    if (failed) {
      result.outcome = SnapshotOutcome::kFailed;
      return result;
    }
  }

  result.outcome =
      warning ? SnapshotOutcome::kDoneWithWarnings : SnapshotOutcome::kDone;
  return result;
}

}  // namespace guacamole_efs
