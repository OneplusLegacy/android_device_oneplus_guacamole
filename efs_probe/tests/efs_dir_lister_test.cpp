#include "efs_dir_lister.h"

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {

// Fake IEfsClient: fully scripted, records call counts AND the exact
// seqno each ReadDir() call requested (requested_seqnos) - the
// 2026-09-23 preflight review specifically requires verifying the
// enumerator's real requested sequence, not just trusting that a
// script's ECHOED seqno happens to look right. Only the
// directory-related methods are exercised by this test file.
class FakeEfsClient : public IEfsClient {
 public:
  EfsOpenDirResult opendir_response;
  std::vector<EfsReadDirResult> readdir_responses;
  EfsCloseDirResult closedir_response;

  int opendir_calls = 0;
  int readdir_calls = 0;
  int closedir_calls = 0;
  std::vector<int32_t> requested_seqnos;

  EfsStatResult Stat(const std::string&) override { return {}; }
  EfsOpenResult OpenReadOnly(const std::string&) override { return {}; }
  EfsReadResult Read(int32_t, uint32_t, uint32_t) override { return {}; }
  EfsCloseResult Close(int32_t) override { return {}; }

  EfsOpenDirResult OpenDir(const std::string&) override {
    opendir_calls++;
    return opendir_response;
  }
  EfsReadDirResult ReadDir(uint32_t, int32_t seqno) override {
    requested_seqnos.push_back(seqno);
    if (static_cast<size_t>(readdir_calls) >= readdir_responses.size()) {
      EfsReadDirResult r;
      r.ok = false;  // ran off the script.
      readdir_calls++;
      return r;
    }
    return readdir_responses[readdir_calls++];
  }
  EfsCloseDirResult CloseDir(uint32_t) override {
    closedir_calls++;
    return closedir_response;
  }
};

EfsOpenDirResult MakeOpenDir(uint32_t dirp = 1, int32_t diag_errno = 0) {
  EfsOpenDirResult o;
  o.ok = true;
  o.dirp = dirp;
  o.diag_errno = diag_errno;
  return o;
}

EfsCloseDirResult MakeCloseDir(int32_t errno_value = 0) {
  EfsCloseDirResult c;
  c.ok = true;
  c.errno_value = errno_value;
  return c;
}

// `seqno` here is what the response ECHOES BACK - deliberately a
// separate parameter from "which position in the script this is",
// since several tests need to script a response whose echoed seqno
// does NOT match what the enumerator actually requested.
EfsReadDirResult MakeReadDir(uint32_t dirp, int32_t seqno,
                              const std::string& name, int32_t diag_errno = 0,
                              int32_t entry_type = 0, int32_t size = 0) {
  EfsReadDirResult r;
  r.ok = true;
  r.dirp = dirp;
  r.seqno = seqno;
  r.diag_errno = diag_errno;
  r.entry_type = entry_type;
  r.mode = 0100644;
  r.size = size;
  r.name = name;
  return r;
}

// The one errno this codebase currently treats as a plausible normal
// end-of-directory signal - see efs_dir_lister.h's header comment.
EfsReadDirResult MakeEof(uint32_t dirp, int32_t seqno) {
  return MakeReadDir(dirp, seqno, "", /*diag_errno=*/kEfsErrDirEntryNotFound);
}

EfsReadDirResult MakeErrorResponse(uint32_t dirp, int32_t seqno,
                                    int32_t diag_errno) {
  return MakeReadDir(dirp, seqno, "", diag_errno);
}

// The H.41 zero-metadata EOD sentinel shape (H41_DEVICE_CONFIRMED,
// third real snapshot-mcfg-readonly attempt - see efs_dir_lister.h's
// header comment): diag_errno=0, every entry field zeroed, empty name.
// Deliberately does NOT reuse MakeReadDir() (which hardcodes
// mode=0100644) - the whole point of this shape is that mode is ALSO
// zero, not just the name.
EfsReadDirResult MakeZeroSentinel(uint32_t dirp, int32_t seqno) {
  EfsReadDirResult r;
  r.ok = true;
  r.dirp = dirp;
  r.seqno = seqno;
  r.diag_errno = 0;
  r.entry_type = 0;
  r.mode = 0;
  r.size = 0;
  r.atime = 0;
  r.mtime = 0;
  r.ctime = 0;
  r.name = "";
  return r;
}

}  // namespace

TEST(EfsDirLister, MultiEntryDirectorySucceeds) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {
      MakeReadDir(1, 1, "mcfg_sel_db.xml", 0, /*entry_type=*/0, 24508),
      MakeReadDir(1, 2, "mcfg_autoselect_by_uim", 0, 0, 4),
      MakeEof(1, 3),
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  ASSERT_EQ(r.entries.size(), 2u);
  EXPECT_EQ(r.entries[0].name, "mcfg_sel_db.xml");
  EXPECT_EQ(r.entries[0].size, 24508);
  EXPECT_EQ(r.entries[1].name, "mcfg_autoselect_by_uim");
  EXPECT_EQ(c.closedir_calls, 1);
  EXPECT_EQ(c.requested_seqnos, (std::vector<int32_t>{1, 2, 3}));
}

TEST(EfsDirLister, EndOfDirectoryDetected) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeEof(1, 1)};  // empty directory - EOF immediately.

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  EXPECT_TRUE(r.entries.empty());
  EXPECT_EQ(c.requested_seqnos, (std::vector<int32_t>{1}));
}

TEST(EfsDirLister, DirectorySequenceAdvancesExactly) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {
      MakeReadDir(1, 1, "a"), MakeReadDir(1, 2, "b"),
      MakeReadDir(1, 3, "c"), MakeEof(1, 4),
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  ASSERT_EQ(r.entries.size(), 3u);
  EXPECT_EQ(r.entries[0].name, "a");
  EXPECT_EQ(r.entries[1].name, "b");
  EXPECT_EQ(r.entries[2].name, "c");
  EXPECT_EQ(c.readdir_calls, 4);  // a, b, c, then the terminating EOF call.
  // The real requested sequence - not just the scripted echo - starts
  // at 1 and advances by exactly 1 each call (2026-09-23 preflight
  // requirement: verify this via the fake's own recorded requests).
  EXPECT_EQ(c.requested_seqnos, (std::vector<int32_t>{1, 2, 3, 4}));
}

TEST(EfsDirLister, NonProgressingSequenceRejected) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  // First call (seqno=1 requested) is answered with a response echoing
  // seqno=5 - an unrelated/stale value, not what was requested.
  c.readdir_responses = {MakeReadDir(1, 5, "unexpected")};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);  // best-effort CLOSEDIR still attempted.
}

TEST(EfsDirLister, DuplicateSequenceRejected) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  // Second call (seqno=2 requested) is answered with a response echoing
  // seqno=1 again - the previously-seen sequence number, not the one
  // actually requested this time.
  c.readdir_responses = {
      MakeReadDir(1, 1, "a"),
      MakeReadDir(1, 1, "a-again"),
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  ASSERT_EQ(r.entries.size(), 0u);  // failure discards the whole listing.
}

TEST(EfsDirLister, ExcessiveEntryCountRejected) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  // Script kMaxDirectoryEntries+1 well-formed, ever-progressing entries
  // (seqno starting at kFirstReadDirSequence) and no EOF at all - the
  // cap must trigger before this ever loops forever.
  for (int32_t i = 0; i < kMaxDirectoryEntries + 1; i++) {
    c.readdir_responses.push_back(
        MakeReadDir(1, kFirstReadDirSequence + i, "entry"));
  }

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_LE(c.readdir_calls, kMaxDirectoryEntries + 1);
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, MalformedEntryRejected) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  EfsReadDirResult malformed;
  malformed.ok = false;
  c.readdir_responses = {malformed};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, ReadDirFailureStillClosesHandle) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  // readdir_responses left empty - the fake's "ran off script" path
  // returns ok=false.

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, OpenDirFailureIsFailClosed) {
  FakeEfsClient c;
  EfsOpenDirResult bad;
  bad.ok = false;
  c.opendir_response = bad;

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.readdir_calls, 0);
  EXPECT_EQ(c.closedir_calls, 0);  // never opened - nothing to close.
}

TEST(EfsDirLister, OpenDirNoEntryIsFailClosed) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1, /*diag_errno=*/50);  // NO_ENTRY-style.

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 0);
}

// --- 2026-09-23 preflight review: READDIR error classification must
// distinguish the one plausible EOF signal from every other genuine
// EFS2 protocol error, which must fail closed rather than be
// misread as a successful (if premature) end of directory. ---

TEST(EfsDirLister, DirEntryNotFoundEndsDirectory) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {
      MakeReadDir(1, 1, "only-entry"),
      MakeErrorResponse(1, 2, kEfsErrDirEntryNotFound),
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].name, "only-entry");
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, InvalidSequenceFailsClosed) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeErrorResponse(1, 1, kEfsErrInvalidSequence)};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_TRUE(r.entries.empty());
  EXPECT_EQ(c.closedir_calls, 1);  // best-effort.
}

TEST(EfsDirLister, DirNotOpenFailsClosed) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeErrorResponse(1, 1, kEfsErrDirNotOpen)};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, InvalidDirEntryFailsClosed) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeErrorResponse(1, 1, kEfsErrInvalidDirEntry)};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, OtherNonzeroReadDirErrorFailsClosed) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(1);
  c.closedir_response = MakeCloseDir();
  // An error code not in the known kEfsErr* set at all - still must
  // fail closed, never default to treating an unrecognized nonzero
  // value as EOF.
  c.readdir_responses = {MakeErrorResponse(1, 1, 0x40000099)};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
  EXPECT_EQ(c.closedir_calls, 1);
}

// --- 2026-09-23 third preflight/real-device finding: H.41's real EOD
// signal for /nv/item_files/mcfg is a zero-metadata sentinel entry
// (diag_errno=0, every field zeroed, empty name), NOT the
// kEfsErrDirEntryNotFound errno path (never observed on this device -
// see efs_dir_lister.h's header comment). ---

TEST(IsZeroSentinelEndOfDirectory, H41RealReadDirZeroSentinelDetected) {
  // The exact decoded shape of the real seq=35 response from the third
  // real snapshot-mcfg-readonly attempt (dirp=2, seqno=35 - the
  // request-level fields are irrelevant to THIS function, which checks
  // only the entry's own shape; see this file's MakeZeroSentinel()).
  auto sentinel = MakeZeroSentinel(2, 35);
  EXPECT_TRUE(IsZeroSentinelEndOfDirectory(sentinel));
}

TEST(IsZeroSentinelEndOfDirectory, EmptyNameButNonzeroModeIsNotEof) {
  auto e = MakeZeroSentinel(1, 1);
  e.mode = 0100644;
  EXPECT_FALSE(IsZeroSentinelEndOfDirectory(e));
}

TEST(IsZeroSentinelEndOfDirectory, EmptyNameButNonzeroSizeIsNotEof) {
  auto e = MakeZeroSentinel(1, 1);
  e.size = 4;
  EXPECT_FALSE(IsZeroSentinelEndOfDirectory(e));
}

TEST(IsZeroSentinelEndOfDirectory, EmptyNameButNonzeroTimestampIsNotEof) {
  auto e = MakeZeroSentinel(1, 1);
  e.atime = 12345;
  EXPECT_FALSE(IsZeroSentinelEndOfDirectory(e));
}

TEST(IsZeroSentinelEndOfDirectory, EmptyNameButNonzeroEntryTypeIsNotEof) {
  auto e = MakeZeroSentinel(1, 1);
  e.entry_type = 1;
  EXPECT_FALSE(IsZeroSentinelEndOfDirectory(e));
}

TEST(IsZeroSentinelEndOfDirectory, NonEmptyNameIsNotEofRegardlessOfZeroFields) {
  auto e = MakeZeroSentinel(1, 1);
  e.name = "a";
  EXPECT_FALSE(IsZeroSentinelEndOfDirectory(e));
}

TEST(EfsDirLister, ZeroSentinelIsNotAddedAsEntry) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {
      MakeReadDir(2, 1, "mcfg_sel_db.xml"),
      MakeZeroSentinel(2, 2),
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].name, "mcfg_sel_db.xml");
}

TEST(EfsDirLister, ZeroSentinelStopsBeforeNextSequence) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {
      MakeReadDir(2, 1, "a"),
      MakeZeroSentinel(2, 2),
      // Deliberately no seq=3 response scripted - if ListDirectory()
      // ever sent a seq=3 request, the fake would return ok=false
      // (ran off script) and the test's own assertions below would
      // catch it via requested_seqnos / readdir_calls, not by this
      // list running out silently.
  };

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  EXPECT_EQ(c.readdir_calls, 2);  // seq=1 (real entry), seq=2 (sentinel) - no seq=3.
  EXPECT_EQ(c.requested_seqnos, (std::vector<int32_t>{1, 2}));
}

TEST(EfsDirLister, ZeroSentinelStillClosesDirectory) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeZeroSentinel(2, 1)};  // empty directory.

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  EXPECT_TRUE(r.entries.empty());
  EXPECT_EQ(c.closedir_calls, 1);
}

TEST(EfsDirLister, ZeroSentinelPreventsEntryCapLoop) {
  // The real /nv/item_files/mcfg shape: 34 real entries (seq 1-34),
  // then the zero sentinel at seq 35 - must stop there, nowhere near
  // kMaxDirectoryEntries (4096).
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  for (int32_t i = 1; i <= 34; i++) {
    c.readdir_responses.push_back(MakeReadDir(2, i, "entry" + std::to_string(i)));
  }
  c.readdir_responses.push_back(MakeZeroSentinel(2, 35));

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  ASSERT_EQ(r.state, ListDirectoryState::kDone);
  EXPECT_EQ(r.entries.size(), 34u);
  EXPECT_EQ(c.readdir_calls, 35);  // 34 real entries + 1 sentinel.
  EXPECT_EQ(c.closedir_calls, 1);
  EXPECT_LT(c.readdir_calls, kMaxDirectoryEntries);
}

TEST(EfsDirLister, WrongDirpOnZeroSentinelIsProtocolFailure) {
  // A zero-shaped entry whose dirp doesn't match the opened handle is
  // NOT a valid EOD signal - it's the existing dirp-correlation
  // fail-closed path, unaffected by zero-sentinel recognition.
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeZeroSentinel(/*dirp=*/99, 1)};

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
}

TEST(EfsDirLister, WrongSeqnoOnZeroSentinelIsProtocolFailure) {
  FakeEfsClient c;
  c.opendir_response = MakeOpenDir(2);
  c.closedir_response = MakeCloseDir();
  c.readdir_responses = {MakeZeroSentinel(2, /*seqno=*/99)};  // requested 1.

  auto r = ListDirectory(&c, "/nv/item_files/mcfg");
  EXPECT_EQ(r.state, ListDirectoryState::kFailed);
}
