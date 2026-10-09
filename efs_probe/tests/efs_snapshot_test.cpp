#include "efs_snapshot.h"
#include "hash.h"
#include "efs_real_client.h"
#include "diag_frame.h"

#include <algorithm>
#include <map>
#include <memory>
#include <chrono>
#include <utility>

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {

EfsStatResult MakeStat(int32_t size, int32_t diag_errno = 0,
                        int32_t mode = 0100644) {
  EfsStatResult s;
  s.ok = true;
  s.diag_errno = diag_errno;
  s.size = size;
  s.mode = mode;
  return s;
}

EfsOpenDirResult MakeOpenDir(uint32_t dirp, int32_t diag_errno = 0) {
  EfsOpenDirResult o;
  o.ok = true;
  o.dirp = dirp;
  o.diag_errno = diag_errno;
  return o;
}

EfsReadDirResult MakeReadDir(uint32_t dirp, int32_t seqno,
                              const std::string& name) {
  EfsReadDirResult r;
  r.ok = true;
  r.dirp = dirp;
  r.seqno = seqno;
  r.diag_errno = 0;
  r.entry_type = 0;
  r.mode = 0100644;
  r.size = 1;
  r.name = name;
  return r;
}

// The one errno this codebase currently treats as a plausible normal
// end-of-directory signal - see efs_dir_lister.h's header comment
// (2026-09-23 preflight correction: was a placeholder value of 2).
EfsReadDirResult MakeEof(uint32_t dirp, int32_t seqno) {
  EfsReadDirResult r = MakeReadDir(dirp, seqno, "");
  r.diag_errno = kEfsErrDirEntryNotFound;
  return r;
}

// Fully scripted, per-path IEfsClient fake. A path with no scripted
// stat entry, or no scripted file contents, behaves as "not found by
// this fake"/"fails" as appropriate - see each accessor below.
class FakeEfsClient : public IEfsClient {
 public:
  std::map<std::string, EfsStatResult> stat_by_path;
  std::map<std::string, std::vector<uint8_t>> contents_by_path;
  std::map<std::string, EfsOpenDirResult> opendir_by_path;
  std::map<uint32_t, std::vector<EfsReadDirResult>> readdir_script_by_dirp;
  std::map<uint32_t, size_t> readdir_pos_by_dirp;
  std::map<int32_t, std::string> fd_to_path;
  int32_t next_fd = 100;
  uint32_t next_dirp = 1;
  std::map<std::string, uint32_t> dirp_by_path;

  int stat_calls = 0, open_calls = 0, read_calls = 0, close_calls = 0;
  int opendir_calls = 0, readdir_calls = 0, closedir_calls = 0;
  std::vector<std::string> touched_paths;

  EfsStatResult Stat(const std::string& path) override {
    stat_calls++;
    touched_paths.push_back(path);
    auto it = stat_by_path.find(path);
    if (it == stat_by_path.end()) {
      EfsStatResult r;
      r.ok = false;
      return r;
    }
    return it->second;
  }

  EfsOpenResult OpenReadOnly(const std::string& path) override {
    open_calls++;
    EfsOpenResult r;
    auto it = contents_by_path.find(path);
    if (it == contents_by_path.end()) {
      r.ok = false;
      return r;
    }
    r.ok = true;
    r.diag_errno = 0;
    r.fd = next_fd;
    fd_to_path[next_fd] = path;
    next_fd++;
    return r;
  }

  EfsReadResult Read(int32_t fd, uint32_t nbyte, uint32_t offset) override {
    read_calls++;
    EfsReadResult r;
    auto path_it = fd_to_path.find(fd);
    if (path_it == fd_to_path.end()) {
      r.ok = false;
      return r;
    }
    const auto& content = contents_by_path[path_it->second];
    if (offset > content.size()) {
      r.ok = false;
      return r;
    }
    uint32_t avail = static_cast<uint32_t>(content.size()) - offset;
    uint32_t n = nbyte < avail ? nbyte : avail;
    r.ok = true;
    r.fd = fd;
    r.offset = offset;
    r.diag_errno = 0;
    r.bytes_read = static_cast<int32_t>(n);
    r.data.assign(content.begin() + offset, content.begin() + offset + n);
    return r;
  }

  EfsCloseResult Close(int32_t fd) override {
    close_calls++;
    fd_to_path.erase(fd);
    EfsCloseResult r;
    r.ok = true;
    r.errno_value = 0;
    return r;
  }

  EfsOpenDirResult OpenDir(const std::string& path) override {
    opendir_calls++;
    touched_paths.push_back(path);
    auto it = opendir_by_path.find(path);
    if (it == opendir_by_path.end()) {
      EfsOpenDirResult r;
      r.ok = false;
      return r;
    }
    dirp_by_path[path] = it->second.dirp;
    return it->second;
  }

  EfsReadDirResult ReadDir(uint32_t dirp, int32_t) override {
    readdir_calls++;
    auto& script = readdir_script_by_dirp[dirp];
    size_t& pos = readdir_pos_by_dirp[dirp];
    if (pos >= script.size()) {
      EfsReadDirResult r;
      r.ok = false;
      return r;
    }
    return script[pos++];
  }

  EfsCloseDirResult CloseDir(uint32_t) override {
    closedir_calls++;
    EfsCloseDirResult r;
    r.ok = true;
    r.errno_value = 0;
    return r;
  }

  // Test setup convenience: mark `path` fully present with `content`.
  void SetFile(const std::string& path, std::vector<uint8_t> content) {
    stat_by_path[path] = MakeStat(static_cast<int32_t>(content.size()));
    contents_by_path[path] = std::move(content);
  }
  // Marks `path`'s STAT as returning a STRUCTURALLY VALID but nonzero
  // `diag_errno` - i.e. "some EFS-level error", NOT a confirmed
  // "file doesn't exist" signal (2026-09-23 preflight correction: this
  // fake used to be named SetFileAbsent/SetDirAbsent and the tests
  // using it asserted TargetPresence::kAbsent - that conflated "STAT
  // returned an error" with "confirmed absent", which is exactly the
  // bug this whole correction fixes; see efs_snapshot.h).
  void SetFileStatError(const std::string& path, int32_t diag_errno = 50) {
    stat_by_path[path] = MakeStat(0, diag_errno);
  }
  void SetDirStatError(const std::string& path, int32_t diag_errno = 50) {
    stat_by_path[path] = MakeStat(0, diag_errno);
  }
  // Directory with a real, scripted entry list (names only matter for
  // the STEP 2 cross-check - see efs_snapshot.cpp's NameSeenInListing).
  void SetDirWithEntries(const std::string& path,
                          const std::vector<std::string>& names) {
    stat_by_path[path] = MakeStat(0, 0, 0040755);  // S_IFDIR.
    uint32_t dirp = next_dirp++;
    opendir_by_path[path] = MakeOpenDir(dirp);
    std::vector<EfsReadDirResult> script;
    int32_t seqno = kFirstReadDirSequence;
    for (const auto& n : names) {
      script.push_back(MakeReadDir(dirp, seqno, n));
      seqno++;
    }
    script.push_back(MakeEof(dirp, seqno));
    readdir_script_by_dirp[dirp] = std::move(script);
  }
  void SetEmptyDir(const std::string& path) { SetDirWithEntries(path, {}); }
};

std::string P(EfsTarget t) { return TargetPath(t); }

// Populates every fixed target as present with minimal valid content -
// tests override exactly one target to exercise a specific behavior.
// The mcfg/ directory listing includes the real basenames of every
// child target (2026-09-23 second whitelist expansion added 5 more),
// so directory_entry_seen is realistically true for them in the
// fully-successful baseline (see
// DirectoryListingCrossCheckDetectsContradiction for the interesting
// case where this diverges from a target's own STAT result).
//
// STEP INDEX MAP (0-based `entries[i]`, matches efs_snapshot.cpp's
// step order exactly - kept here as the single reference other tests
// below index against):
//   0  STAT mcfg               1  LIST mcfg
//   2  sel_db.xml               3  autoselect
//   4  autoselect_Subscription01
//   5  setting                  6  setting_1
//   7  setting_Subscription01   8  setting_Subscription01_1
//   9  rfs_sw_digest_version   10  cards (LIST)
//  11  debug.log               12  debug_old.log
//  13  enable_mcfg_efslogging
constexpr size_t kIdxStatMcfg = 0;
constexpr size_t kIdxListMcfg = 1;
constexpr size_t kIdxSelDb = 2;
constexpr size_t kIdxAutoselect = 3;
constexpr size_t kIdxAutoselectSub01 = 4;
constexpr size_t kIdxSetting = 5;
constexpr size_t kIdxSetting1 = 6;
constexpr size_t kIdxSettingSub01 = 7;
constexpr size_t kIdxSettingSub01_1 = 8;
constexpr size_t kIdxRfsDigest = 9;
constexpr size_t kIdxCards = 10;
constexpr size_t kIdxDebugLog = 11;
constexpr size_t kIdxDebugOldLog = 12;
constexpr size_t kIdxEfslogging = 13;
constexpr size_t kTotalSteps = 14;

void PopulateFullSuccess(FakeEfsClient* c) {
  c->SetDirWithEntries(
      P(EfsTarget::kMcfgDir),
      {"mcfg_sel_db.xml", "mcfg_autoselect_by_uim",
       "mcfg_autoselect_by_uim_Subscription01", "mcfg_setting",
       "mcfg_setting_1", "mcfg_setting_Subscription01",
       "mcfg_setting_Subscription01_1", "mcfg_rfs_sw_digest_version",
       "cards"});
  c->SetFile(P(EfsTarget::kMcfgSelDb), {'<', 'x', 'm', 'l', '/', '>'});
  c->SetFile(P(EfsTarget::kMcfgAutoselectByUim), {0x05});
  c->SetFile(P(EfsTarget::kMcfgAutoselectByUimSubscription01), {0x06});
  c->SetFile(P(EfsTarget::kMcfgSetting), {0x01});
  c->SetFile(P(EfsTarget::kMcfgSetting1), {0x02});
  c->SetFile(P(EfsTarget::kMcfgSettingSubscription01), {0x03});
  c->SetFile(P(EfsTarget::kMcfgSettingSubscription01_1), {0x04});
  c->SetFile(P(EfsTarget::kMcfgRfsSwDigestVersion), {0x00});
  c->SetEmptyDir(P(EfsTarget::kMcfgCards));
  c->SetFile(P(EfsTarget::kMcfgDebugLog), {'l', 'o', 'g'});
  c->SetFile(P(EfsTarget::kMcfgDebugOldLog), {'o', 'l', 'd'});
  c->SetFile(P(EfsTarget::kEnableMcfgEfsLogging), {0x00});
}

}  // namespace

TEST(EfsSnapshot, FullSuccessAllPresent) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
  ASSERT_EQ(r.entries.size(), kTotalSteps);
  for (const auto& e : r.entries) {
    EXPECT_EQ(e.presence, TargetPresence::kExists) << e.step_name;
  }
  EXPECT_TRUE(r.entries[kIdxSelDb].has_sha256);
  // Every mcfg/ child is named in STEP 2's own listing.
  EXPECT_TRUE(r.entries[kIdxSelDb].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxAutoselect].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxAutoselectSub01].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxSetting].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxSetting1].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxSettingSub01].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxSettingSub01_1].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxRfsDigest].directory_entry_seen);
  EXPECT_TRUE(r.entries[kIdxCards].directory_entry_seen);
  // The 3 targets outside mcfg/ never get the cross-check.
  EXPECT_FALSE(r.entries[kIdxDebugLog].directory_entry_seen);
  EXPECT_FALSE(r.entries[kIdxDebugOldLog].directory_entry_seen);
  EXPECT_FALSE(r.entries[kIdxEfslogging].directory_entry_seen);
}

TEST(EfsSnapshot, SnapshotUsesOnlyFixedTargets) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);

  std::vector<std::string> whitelist;
  for (int i = 0;
       i <= static_cast<int>(EfsTarget::kMcfgSettingSubscription01_1); i++) {
    whitelist.push_back(TargetPath(static_cast<EfsTarget>(i)));
  }
  for (const auto& touched : c.touched_paths) {
    EXPECT_NE(std::find(whitelist.begin(), whitelist.end(), touched),
              whitelist.end())
        << "touched a non-whitelisted path: " << touched;
  }
}

TEST(EfsSnapshot, MandatoryStatErrorStopsSnapshot) {
  FakeEfsClient c;
  c.SetDirStatError(P(EfsTarget::kMcfgDir));

  auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  ASSERT_EQ(r.entries.size(), 1u);
  // CORRECTED 2026-09-23: a STAT error does NOT prove absence -
  // presence must be kUnknown, never kAbsent.
  EXPECT_EQ(r.entries[0].presence, TargetPresence::kUnknown);
  EXPECT_EQ(c.opendir_calls, 0);  // STEP 2 never ran.
  EXPECT_EQ(c.stat_calls, 1);     // no later steps attempted either.
}

TEST(EfsSnapshot, MandatoryStatErrorFailsWithoutClaimingAbsent) {
  FakeEfsClient c;
  c.SetDirStatError(P(EfsTarget::kMcfgDir), /*diag_errno=*/50);

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kFailed);
  EXPECT_EQ(r.entries[0].presence, TargetPresence::kUnknown);
  // Neither the per-entry message nor the top-level fail_reason may
  // assert "absent" - this code does not know that.
  EXPECT_EQ(r.entries[0].message.find("absent"), std::string::npos);
  EXPECT_EQ(r.fail_reason.find("absent"), std::string::npos);
  EXPECT_NE(r.fail_reason.find("EFS error"), std::string::npos);
  EXPECT_NE(r.fail_reason.find("50"), std::string::npos);
}

TEST(EfsSnapshot, OptionalStatErrorIsUnknownNotAbsent) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSelDb));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_GT(r.entries.size(), kIdxSelDb);
  EXPECT_EQ(r.entries[kIdxSelDb].presence, TargetPresence::kUnknown);
  EXPECT_FALSE(r.entries[kIdxSelDb].has_data);
  EXPECT_EQ(r.entries[kIdxSelDb].message.find("absent"), std::string::npos);
}

TEST(EfsSnapshot, OptionalStatErrorContinuesSnapshot) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSelDb));

  auto r = RunMcfgReadonlySnapshot(&c);
  // A protocol-error-but-continued optional step is a WARNING, not a
  // silent kDone and not a full kFailed.
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  ASSERT_EQ(r.entries.size(), kTotalSteps);
  // Every later step still ran.
  EXPECT_EQ(r.entries[kIdxEfslogging].presence, TargetPresence::kExists);
}

TEST(EfsSnapshot, OptionalSelDbMissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSelDb));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  ASSERT_EQ(r.entries.size(), kTotalSteps);
  EXPECT_EQ(r.entries[kIdxSelDb].presence, TargetPresence::kUnknown);
  EXPECT_FALSE(r.entries[kIdxSelDb].has_data);
  // Every later step still ran.
  EXPECT_EQ(r.entries[kIdxEfslogging].presence, TargetPresence::kExists);
}

TEST(EfsSnapshot, OptionalAutoselectMissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgAutoselectByUim));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxAutoselect].presence, TargetPresence::kUnknown);
}

TEST(EfsSnapshot, OptionalAutoselectSubscription01MissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgAutoselectByUimSubscription01));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxAutoselectSub01].presence,
            TargetPresence::kUnknown);
  EXPECT_EQ(r.entries[kIdxEfslogging].presence, TargetPresence::kExists);
}

TEST(EfsSnapshot, OptionalSettingMissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSetting));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxSetting].presence, TargetPresence::kUnknown);
}

TEST(EfsSnapshot, OptionalSetting1MissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSetting1));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxSetting1].presence, TargetPresence::kUnknown);
}

TEST(EfsSnapshot, OptionalSettingSubscription01MissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSettingSubscription01));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxSettingSub01].presence, TargetPresence::kUnknown);
}

TEST(EfsSnapshot, OptionalSettingSubscription01_1MissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSettingSubscription01_1));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxSettingSub01_1].presence, TargetPresence::kUnknown);
  EXPECT_EQ(r.entries[kIdxEfslogging].presence, TargetPresence::kExists);
}

TEST(EfsSnapshot, OptionalDigestMissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgRfsSwDigestVersion));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxRfsDigest].presence, TargetPresence::kUnknown);
}

TEST(EfsSnapshot, OptionalCardsMissingContinues) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.opendir_by_path.erase(P(EfsTarget::kMcfgCards));
  c.SetDirStatError(P(EfsTarget::kMcfgCards));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_EQ(r.entries[kIdxCards].presence, TargetPresence::kUnknown);
  // Later file steps still ran.
  EXPECT_EQ(r.entries[kIdxDebugLog].presence, TargetPresence::kExists);
}

TEST(EfsSnapshot, DirectoryListingCrossCheckDetectsContradiction) {
  // sel_db.xml IS named in STEP 2's own directory listing, but its own
  // STAT call independently returns an EFS error - a genuine
  // contradiction between two different signals that Cycle 2 needs
  // preserved, not silently resolved either way by this code.
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  c.SetFileStatError(P(EfsTarget::kMcfgSelDb));

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  const auto& sel_db_entry = r.entries[kIdxSelDb];
  EXPECT_EQ(sel_db_entry.presence, TargetPresence::kUnknown);
  EXPECT_TRUE(sel_db_entry.directory_entry_seen);
}

TEST(EfsSnapshot, DirectoryListingCrossCheckFalseWhenNotListed) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  // Override STEP 2's own listing to OMIT sel_db.xml, even though
  // sel_db.xml itself still reads back successfully via its own
  // already-populated SetFile() call above.
  c.SetDirWithEntries(
      P(EfsTarget::kMcfgDir),
      {"mcfg_autoselect_by_uim", "mcfg_autoselect_by_uim_Subscription01",
       "mcfg_setting", "mcfg_setting_1", "mcfg_setting_Subscription01",
       "mcfg_setting_Subscription01_1", "mcfg_rfs_sw_digest_version",
       "cards"});

  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
  EXPECT_EQ(r.entries[kIdxSelDb].presence, TargetPresence::kExists);
  EXPECT_FALSE(r.entries[kIdxSelDb].directory_entry_seen);
}

TEST(EfsSnapshot, NoCallerSuppliedPaths) {
  // RunMcfgReadonlySnapshot() has no path/string parameter of any kind
  // besides the IEfsClient* itself - this is a compile-time guarantee;
  // this test corroborates it at runtime by confirming two independent
  // fakes given the SAME script produce byte-identical touched-path
  // sequences, since there is no way to make it vary.
  FakeEfsClient c1;
  PopulateFullSuccess(&c1);
  auto r1 = RunMcfgReadonlySnapshot(&c1);

  FakeEfsClient c2;
  PopulateFullSuccess(&c2);
  auto r2 = RunMcfgReadonlySnapshot(&c2);

  EXPECT_EQ(c1.touched_paths, c2.touched_paths);
  EXPECT_EQ(r1.outcome, r2.outcome);
}

TEST(EfsSnapshot, FirstTransportFailureStopsLaterSteps) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  // Make the autoselect STAT structurally malformed (ok=false) - a
  // genuine transport/protocol failure, not an "absent" signal.
  EfsStatResult bad;
  bad.ok = false;
  c.stat_by_path[P(EfsTarget::kMcfgAutoselectByUim)] = bad;

  auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  // STEP 1, STEP 2, STEP 3 (sel_db, succeeded), STEP 4 (autoselect,
  // failed) - and nothing after.
  ASSERT_EQ(r.entries.size(), 4u);
  EXPECT_NE(r.fail_reason.find("STAT+READ mcfg_autoselect_by_uim"),
            std::string::npos);
}

TEST(EfsSnapshot, NoAutomaticRetry) {
  FakeEfsClient c;
  PopulateFullSuccess(&c);
  EfsStatResult bad;
  bad.ok = false;
  c.stat_by_path[P(EfsTarget::kMcfgSelDb)] = bad;

  auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  // Exactly one STAT attempt for the failing target - no retry loop.
  int sel_db_stat_calls = 0;
  for (const auto& p : c.touched_paths) {
    if (p == P(EfsTarget::kMcfgSelDb)) sel_db_stat_calls++;
  }
  EXPECT_EQ(sel_db_stat_calls, 1);
}

// Round 2.5: exercise the actual snapshot, never a public raw-path reader.
namespace {
std::string MuxdHex(const std::vector<uint8_t>& bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; }
  return out;
}
const std::string kMuxdName = "mcfg_sw_muxd_version_39";
std::string MuxdPath(const std::string& name = kMuxdName) {
  return P(EfsTarget::kMcfgDir) + "/" + name;
}
void AddMuxd(FakeEfsClient* c, const std::string& name = kMuxdName,
             int32_t type = 15, int32_t mode = 0160777,
             std::vector<uint8_t> bytes = {1, 2, 3, 4}) {
  c->SetFile(MuxdPath(name), bytes);
  c->stat_by_path[MuxdPath(name)].mode = mode;
  auto dirp = c->opendir_by_path[P(EfsTarget::kMcfgDir)].dirp;
  auto& script = c->readdir_script_by_dirp[dirp];
  auto end = script.back(); script.pop_back();
  auto entry = MakeReadDir(dirp, end.seqno, name);
  entry.entry_type = type; entry.mode = mode; entry.size = bytes.size();
  script.push_back(entry); ++end.seqno; script.push_back(end);
}
class MuxdFailureClient : public FakeEfsClient {
 public:
  enum Failure { kPartial, kTimeout, kWrongBytes, kClose, kChangedStat };
  Failure failure = kPartial;
  int muxd_reads = 0, muxd_opens = 0, muxd_stats = 0;
  EfsOpenResult OpenReadOnly(const std::string& path) override {
    if (path == MuxdPath()) ++muxd_opens;
    return FakeEfsClient::OpenReadOnly(path);
  }
  EfsStatResult Stat(const std::string& path) override {
    auto r = FakeEfsClient::Stat(path);
    if (path == MuxdPath() && ++muxd_stats == 2 && failure == kChangedStat) ++r.size;
    return r;
  }
  EfsReadResult Read(int32_t fd, uint32_t count, uint32_t offset) override {
    auto r = FakeEfsClient::Read(fd, count, offset);
    if (fd_to_path[fd] == MuxdPath()) {
      ++muxd_reads;
      if (failure == kTimeout) return {};
      if (failure == kPartial) { --r.bytes_read; r.data.pop_back(); }
      if (failure == kWrongBytes) r.data.pop_back();
    }
    return r;
  }
  EfsCloseResult Close(int32_t fd) override {
    const bool muxd = fd_to_path[fd] == MuxdPath();
    auto r = FakeEfsClient::Close(fd);
    if (muxd && failure == kClose) r.ok = false;
    return r;
  }
};
}
TEST(EfsSnapshotMuxd, ObservedBasenamesAndTypesAcceptedWithExactBytesAndHash) {
  FakeEfsClient c; PopulateFullSuccess(&c);
  const std::vector<std::string> names = {"mcfg_sw_muxd_version",
      "mcfg_sw_muxd_version_8", "mcfg_sw_muxd_version_151",
      "mcfg_sw_muxd_version_Subscription01",
      "mcfg_sw_muxd_version_Subscription01_39"};
  for (const auto& name : names) AddMuxd(&c, name);
  const auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
  ASSERT_EQ(r.entries.size(), kTotalSteps + names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    const auto& e = r.entries[kTotalSteps + i];
    EXPECT_EQ(e.discovered_name, names[i]);
    EXPECT_EQ(e.discovered_path, MuxdPath(names[i]));
    EXPECT_TRUE(e.directory_entry_seen);
    EXPECT_EQ(e.listed_size, 4); EXPECT_EQ(e.stat_size, 4);
    EXPECT_EQ(e.collection_status, "VERIFIED");
    EXPECT_EQ(e.data, (std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_TRUE(e.has_sha256);
    EXPECT_EQ(MuxdHex(e.data), "01020304");
    EXPECT_EQ(MuxdHex(std::vector<uint8_t>(e.sha256.begin(), e.sha256.end())),
        "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a");
  }
}
TEST(EfsSnapshotMuxd, RegularFileAcceptedToo) {
  FakeEfsClient c; PopulateFullSuccess(&c); AddMuxd(&c, kMuxdName, 0, 0100644);
  const auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
  EXPECT_EQ(r.entries.back().collection_status, "VERIFIED");
}
TEST(EfsSnapshotMuxd, UnsafeAndUnrelatedNamesNeverRead) {
  for (const std::string name : {"unrelated", "../mcfg_sw_muxd_version_39",
       "mcfg_sw_muxd_version/../secret", "mcfg_sw_muxd_version_evil",
       "mcfg_sw_muxd_version39", "mcfg_sw_muxd_version_",
       "mcfg_sw_muxd_version_Subscription01_", "mcfg_sw_muxd_version_39/child",
       "mcfg_sw_muxd_version_39\n", "mcfg_sw_muxd_version_12345678901"}) {
    FakeEfsClient c; PopulateFullSuccess(&c); AddMuxd(&c, name);
    auto r = RunMcfgReadonlySnapshot(&c);
    EXPECT_NE(r.outcome, SnapshotOutcome::kFailed);
    EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath(name)), 0);
    for (const auto& e : r.entries) EXPECT_NE(e.collection_status, "VERIFIED");
  }
}
TEST(EfsSnapshotMuxd, EmbeddedNullNeverRead) {
  FakeEfsClient c; PopulateFullSuccess(&c);
  const std::string name = kMuxdName + std::string("\0x", 2);
  AddMuxd(&c, name); auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
  EXPECT_FALSE(r.entries.back().has_data);
}
TEST(EfsSnapshotMuxd, ValidButNotEnumeratedCannotBeRead) {
  FakeEfsClient c; PopulateFullSuccess(&c); c.SetFile(MuxdPath(), {1, 2, 3, 4});
  const auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.entries.size(), kTotalSteps);
  EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath()), 0);
  EXPECT_FALSE(ResolveTargetName(kMuxdName));
  EXPECT_FALSE(ResolveTargetName(MuxdPath()));
}
TEST(EfsSnapshotMuxd, DirectorySymlinkUnknownAndInconsistentTypesSkipped) {
  for (auto pair : {std::pair<int32_t, int32_t>{1, 0040755}, {2, 0120777},
                    {15, 0120777}, {0, 0160777}, {42, 0100644}}) {
    FakeEfsClient c; PopulateFullSuccess(&c); AddMuxd(&c, kMuxdName, pair.first, pair.second);
    auto r = RunMcfgReadonlySnapshot(&c);
    EXPECT_EQ(r.outcome, SnapshotOutcome::kDoneWithWarnings);
    EXPECT_EQ(r.entries.back().collection_status, "SKIPPED");
    EXPECT_FALSE(r.entries.back().has_sha256);
    EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath()), 0);
  }
}
TEST(EfsSnapshotMuxd, OversizedListingRejectedBeforeStatOrOpen) {
  FakeEfsClient c; PopulateFullSuccess(&c);
  AddMuxd(&c, kMuxdName, 15, 0160777, std::vector<uint8_t>(4097, 1));
  const auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath()), 0);
  EXPECT_FALSE(r.entries.back().has_sha256);
}
TEST(EfsSnapshotMuxd, ListingStatSizeOrTypeMismatchRejectedBeforeOpen) {
  for (bool change_size : {false, true}) {
    MuxdFailureClient c; PopulateFullSuccess(&c); AddMuxd(&c);
    if (change_size) c.stat_by_path[MuxdPath()].size = 4097;
    else c.stat_by_path[MuxdPath()].mode = 0120777;
    EXPECT_EQ(RunMcfgReadonlySnapshot(&c).outcome, SnapshotOutcome::kFailed);
    EXPECT_EQ(c.muxd_opens, 0);
  }
}
TEST(EfsSnapshotMuxd, PartialTimeoutWrongBytesCloseOrPostStatFailureNeverHashOrRetry) {
  for (auto failure : {MuxdFailureClient::kPartial, MuxdFailureClient::kTimeout,
       MuxdFailureClient::kWrongBytes, MuxdFailureClient::kClose,
       MuxdFailureClient::kChangedStat}) {
    MuxdFailureClient c; c.failure = failure; PopulateFullSuccess(&c); AddMuxd(&c);
    AddMuxd(&c, "mcfg_sw_muxd_version_40");
    auto r = RunMcfgReadonlySnapshot(&c);
    EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
    EXPECT_FALSE(r.entries.back().has_data); EXPECT_FALSE(r.entries.back().has_sha256);
    EXPECT_EQ(r.entries.back().presence, TargetPresence::kUnknown);
    EXPECT_EQ(c.muxd_reads, 1); EXPECT_EQ(c.muxd_opens, 1);
    EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(),
                         MuxdPath("mcfg_sw_muxd_version_40")), 0);
  }
}
TEST(EfsSnapshotMuxd, DuplicateNamesFailBeforeAnyMuxdRead) {
  FakeEfsClient c; PopulateFullSuccess(&c); AddMuxd(&c); AddMuxd(&c);
  EXPECT_EQ(RunMcfgReadonlySnapshot(&c).outcome, SnapshotOutcome::kFailed);
  EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath()), 0);
}
TEST(EfsSnapshotMuxd, EmptyAndCapSizedFilesReadCompletely) {
  for (size_t n : {0u, 4096u}) {
    FakeEfsClient c; PopulateFullSuccess(&c);
    AddMuxd(&c, kMuxdName, 15, 0160777, std::vector<uint8_t>(n, 0x7e));
    auto r = RunMcfgReadonlySnapshot(&c);
    ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
    EXPECT_EQ(r.entries.back().data.size(), n);
    EXPECT_TRUE(r.entries.back().has_sha256);
  }
}
TEST(EfsSnapshotMuxd, FileCountCapFailsBeforeAnyMuxdRead) {
  FakeEfsClient c; PopulateFullSuccess(&c);
  for (int i = 0; i < 257; ++i) AddMuxd(&c, "mcfg_sw_muxd_version_" + std::to_string(i));
  EXPECT_EQ(RunMcfgReadonlySnapshot(&c).outcome, SnapshotOutcome::kFailed);
  EXPECT_EQ(std::count(c.touched_paths.begin(), c.touched_paths.end(), MuxdPath()), 0);
}

namespace {
// Real EfsRealClient + strict DiagTransport for muxd, fake fixed-target data.
// The fake DIAG endpoint rejects every command except STAT/OPEN/READ/CLOSE.
class MuxdWireShim : public IDiagCallbackShim {
 public:
  DiagDataCallback callback = nullptr;
  void* context = nullptr;
  std::vector<uint16_t> commands;
  bool timeout_read = false, corrupt_read = false;
  bool Init(std::string*) override { return true; }
  void Deinit() override {}
  bool RegisterCallback(DiagDataCallback cb, void* ctx, std::string*) override {
    callback = cb; context = ctx; return true;
  }
  bool SwitchLogging(int mode, std::string*) override { return mode == kDiagCallbackMode; }
  static uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  bool SendData(int, const void* buf, int len, std::string*) override {
    if (len < 4) return false;
    const auto* p = static_cast<const uint8_t*>(buf);
    const uint16_t cmd = uint16_t(p[2]) | (uint16_t(p[3]) << 8);
    commands.push_back(cmd);
    std::vector<uint8_t> r = {0x4b, 0x13, p[2], p[3]};
    auto u32 = [&](uint32_t v) {
      for (int i = 0; i < 4; ++i) r.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    switch (cmd) {
      case kEfsCmdStat:
        u32(0); u32(0160777); u32(4); u32(1); u32(0); u32(0); u32(0);
        break;
      case kEfsCmdOpen:
        if (len < 12) return false;
        EXPECT_EQ(U32(p + 4), 0u);  // O_RDONLY, never O_TRUNC or O_CREAT
        EXPECT_EQ(U32(p + 8), 0u);  // no creation mode
        EXPECT_EQ(std::vector<uint8_t>(p, p + len),
                  BuildOpenReadOnlyRequest(MuxdPath(), kOflagReadOnly));
        u32(7); u32(0);
        break;
      case kEfsCmdRead:
        if (timeout_read) return true;  // no callback; actual transport times out
        if (len != 16) return false;
        EXPECT_EQ(U32(p + 4), 7u); EXPECT_EQ(U32(p + 8), 4u); EXPECT_EQ(U32(p + 12), 0u);
        u32(7); u32(0); u32(4); u32(0);
        r.insert(r.end(), {1, 2, 3, 4});
        break;
      case kEfsCmdClose:
        u32(0); break;
      default:
        ADD_FAILURE() << "Unexpected/mutating EFS command " << cmd;
        return false;
    }
    uint16_t crc = Crc16X25(r.data(), r.size());
    if (cmd == kEfsCmdRead && corrupt_read) crc ^= 1;
    r.push_back(crc & 0xff); r.push_back(crc >> 8);
    std::vector<uint8_t> framed;
    for (uint8_t b : r) {
      if (b == 0x7d || b == 0x7e) { framed.push_back(0x7d); framed.push_back(b ^ 0x20); }
      else framed.push_back(b);
    }
    framed.push_back(0x7e);
    callback(framed.data(), framed.size(), context);
    return true;
  }
};
class MuxdWireClient : public FakeEfsClient {
 public:
  explicit MuxdWireClient(EfsRealClient& real) : real_(real) {}
  EfsStatResult Stat(const std::string& p) override {
    return p == MuxdPath() ? real_.Stat(p) : FakeEfsClient::Stat(p);
  }
  EfsOpenResult OpenReadOnly(const std::string& p) override {
    return p == MuxdPath() ? real_.OpenReadOnly(p) : FakeEfsClient::OpenReadOnly(p);
  }
  EfsReadResult Read(int32_t fd, uint32_t n, uint32_t off) override {
    return fd == 7 ? real_.Read(fd, n, off) : FakeEfsClient::Read(fd, n, off);
  }
  EfsCloseResult Close(int32_t fd) override {
    return fd == 7 ? real_.Close(fd) : FakeEfsClient::Close(fd);
  }
 private:
  EfsRealClient& real_;
};
}
TEST(EfsSnapshotMuxd, RealClientWireUsesOnlyReadonlyCommandsAndFullBytes) {
  auto owned = std::make_unique<MuxdWireShim>();
  auto& shim = *owned; DiagTransport transport(std::move(owned));
  std::string error; ASSERT_TRUE(transport.Init(&error));
  EfsRealClient real(&transport, std::chrono::milliseconds(1));
  MuxdWireClient c(real); PopulateFullSuccess(&c); AddMuxd(&c);
  auto r = RunMcfgReadonlySnapshot(&c);
  ASSERT_EQ(r.outcome, SnapshotOutcome::kDone);
  EXPECT_EQ(MuxdHex(r.entries.back().data), "01020304");
  EXPECT_EQ(shim.commands, (std::vector<uint16_t>{kEfsCmdStat, kEfsCmdOpen,
      kEfsCmdRead, kEfsCmdClose, kEfsCmdStat}));
}
TEST(EfsSnapshotMuxd, RealTransportTimeoutPoisonsNoRetryOrLaterWireCall) {
  auto owned = std::make_unique<MuxdWireShim>();
  auto& shim = *owned; shim.timeout_read = true;
  DiagTransport transport(std::move(owned));
  std::string error; ASSERT_TRUE(transport.Init(&error));
  EfsRealClient real(&transport, std::chrono::milliseconds(1));
  MuxdWireClient c(real); PopulateFullSuccess(&c); AddMuxd(&c);
  auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  EXPECT_FALSE(r.entries.back().has_sha256);
  EXPECT_EQ(shim.commands, (std::vector<uint16_t>{kEfsCmdStat, kEfsCmdOpen, kEfsCmdRead}));
}
TEST(EfsSnapshotMuxd, RealTransportCrcFailurePoisonsWithoutHashOrRetry) {
  auto owned = std::make_unique<MuxdWireShim>();
  auto& shim = *owned; shim.corrupt_read = true;
  DiagTransport transport(std::move(owned));
  std::string error; ASSERT_TRUE(transport.Init(&error));
  EfsRealClient real(&transport, std::chrono::milliseconds(1));
  MuxdWireClient c(real); PopulateFullSuccess(&c); AddMuxd(&c);
  auto r = RunMcfgReadonlySnapshot(&c);
  EXPECT_EQ(r.outcome, SnapshotOutcome::kFailed);
  EXPECT_FALSE(r.entries.back().has_sha256);
  EXPECT_EQ(shim.commands, (std::vector<uint16_t>{kEfsCmdStat, kEfsCmdOpen, kEfsCmdRead}));
}
