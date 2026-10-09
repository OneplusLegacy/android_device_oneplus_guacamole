#include "efs_reader.h"

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {

// Fake IEfsClient: fully scripted, records call counts so tests can
// assert "no retry" directly rather than inferring it from timing.
class FakeEfsClient : public IEfsClient {
 public:
  EfsStatResult stat_response;
  EfsOpenResult open_response;
  std::vector<EfsReadResult> read_responses;
  EfsCloseResult close_response;

  int stat_calls = 0;
  int open_calls = 0;
  int read_calls = 0;
  int close_calls = 0;

  EfsStatResult Stat(const std::string&) override {
    stat_calls++;
    return stat_response;
  }
  EfsOpenResult OpenReadOnly(const std::string&) override {
    open_calls++;
    return open_response;
  }
  EfsReadResult Read(int32_t, uint32_t, uint32_t) override {
    if (static_cast<size_t>(read_calls) >= read_responses.size()) {
      EfsReadResult r;
      r.ok = false;  // ran off the script - treat as a hard failure, not
                      // an implicit retry-forever loop.
      read_calls++;
      return r;
    }
    return read_responses[read_calls++];
  }
  EfsCloseResult Close(int32_t) override {
    close_calls++;
    return close_response;
  }
  EfsOpenDirResult OpenDir(const std::string&) override { return {}; }
  EfsReadDirResult ReadDir(uint32_t, int32_t) override { return {}; }
  EfsCloseDirResult CloseDir(uint32_t) override { return {}; }
};

EfsStatResult MakeStat(int32_t size, int32_t diag_errno = 0,
                        int32_t mode = 0100644) {
  EfsStatResult s;
  s.ok = true;
  s.diag_errno = diag_errno;
  s.size = size;
  s.mode = mode;
  return s;
}

EfsOpenResult MakeOpen(int32_t fd = 3, int32_t diag_errno = 0) {
  EfsOpenResult o;
  o.ok = true;
  o.fd = fd;
  o.diag_errno = diag_errno;
  return o;
}

EfsCloseResult MakeClose(int32_t errno_value = 0) {
  EfsCloseResult c;
  c.ok = true;
  c.errno_value = errno_value;
  return c;
}

EfsReadResult MakeRead(int32_t fd, uint32_t offset, std::vector<uint8_t> data,
                        int32_t diag_errno = 0) {
  EfsReadResult r;
  r.ok = true;
  r.fd = fd;
  r.offset = offset;
  r.bytes_read = static_cast<int32_t>(data.size());
  r.diag_errno = diag_errno;
  r.data = std::move(data);
  return r;
}

}  // namespace

TEST(EfsReader, EmptyFile) {
  FakeEfsClient c;
  c.stat_response = MakeStat(0);
  c.open_response = MakeOpen();
  c.close_response = MakeClose();

  auto r = ReadWholeFile(&c, "/a");
  ASSERT_EQ(r.state, EfsReadFileState::kDone);
  EXPECT_EQ(r.total_bytes_read, 0u);
  EXPECT_TRUE(r.data.empty());
  EXPECT_EQ(c.read_calls, 0);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsReader, Exact1024Boundary) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.read_responses = {MakeRead(5, 0, std::vector<uint8_t>(1024, 0xAB))};

  auto r = ReadWholeFile(&c, "/a");
  ASSERT_EQ(r.state, EfsReadFileState::kDone);
  EXPECT_EQ(r.total_bytes_read, 1024u);
  EXPECT_EQ(r.data.size(), 1024u);
  EXPECT_EQ(c.read_calls, 1);
}

TEST(EfsReader, ShortFinalChunk) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1200);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.read_responses = {
      MakeRead(5, 0, std::vector<uint8_t>(1024, 0x01)),
      MakeRead(5, 1024, std::vector<uint8_t>(176, 0x02)),
  };

  auto r = ReadWholeFile(&c, "/a");
  ASSERT_EQ(r.state, EfsReadFileState::kDone);
  EXPECT_EQ(r.total_bytes_read, 1200u);
  EXPECT_EQ(c.read_calls, 2);
}

TEST(EfsReader, MultiChunkFile) {
  FakeEfsClient c;
  c.stat_response = MakeStat(2500);
  c.open_response = MakeOpen(7);
  c.close_response = MakeClose();
  c.read_responses = {
      MakeRead(7, 0, std::vector<uint8_t>(1024, 0x01)),
      MakeRead(7, 1024, std::vector<uint8_t>(1024, 0x02)),
      MakeRead(7, 2048, std::vector<uint8_t>(452, 0x03)),
  };

  auto r = ReadWholeFile(&c, "/a");
  ASSERT_EQ(r.state, EfsReadFileState::kDone);
  EXPECT_EQ(r.total_bytes_read, 2500u);
  EXPECT_EQ(r.data.size(), 2500u);
  EXPECT_EQ(c.read_calls, 3);
}

// The loop's own subtraction invariant (remaining -= bytes_read, next
// chunk capped at remaining) makes total_bytes_read == stat size a
// structural guarantee on any path that completes the loop - so the
// only way to observe "stat size vs. actual bytes" enforcement is via a
// modem that under-delivers mid-file: the loop asks for the remaining
// 24 bytes after a short first chunk, and the script has nothing left
// to answer with, which must fail closed rather than accept the
// 1000 bytes already collected as if they were the whole file.
TEST(EfsReader, StatSizeAndTotalBytesMustMatch) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.read_responses = {MakeRead(5, 0, std::vector<uint8_t>(1000, 0xAB))};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.read_calls, 2);
}

TEST(EfsReader, StatFailureIsFailClosed) {
  FakeEfsClient c;
  EfsStatResult bad;
  bad.ok = false;
  c.stat_response = bad;

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.open_calls, 0);
}

TEST(EfsReader, StatOnDirectoryIsRejected) {
  FakeEfsClient c;
  c.stat_response = MakeStat(4096, 0, /*mode=*/0040755);  // S_IFDIR

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.open_calls, 0);
}

TEST(EfsReader, EfsErrorIsNotRetried) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  EfsReadResult err_read = MakeRead(5, 0, {});
  err_read.diag_errno = 5;  // some real EFS2 error code
  c.read_responses = {err_read};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.read_calls, 1);  // exactly one attempt - no retry.
}

TEST(EfsReader, TimeoutIsNotRetried) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  EfsReadResult timeout_read;
  timeout_read.ok = false;  // a timeout/no-response is surfaced by the
                             // transport layer as a parse/decode failure
                             // at this interface boundary - `ok=false`,
                             // exactly like any other malformed response.
  c.read_responses = {timeout_read};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.read_calls, 1);  // exactly one attempt - no retry.
}

TEST(EfsReader, OffsetAdvancesExactly) {
  FakeEfsClient c;
  c.stat_response = MakeStat(2500);
  c.open_response = MakeOpen(7);
  c.close_response = MakeClose();
  c.read_responses = {
      MakeRead(7, 0, std::vector<uint8_t>(1024, 0x01)),
      MakeRead(7, 1024, std::vector<uint8_t>(1024, 0x02)),
      MakeRead(7, 2048, std::vector<uint8_t>(452, 0x03)),
  };

  auto r = ReadWholeFile(&c, "/a");
  ASSERT_EQ(r.state, EfsReadFileState::kDone);
  ASSERT_EQ(r.data.size(), 2500u);
  // Each chunk's bytes land at its own exact offset in the
  // concatenated result - no overlap, no gap.
  EXPECT_EQ(r.data[0], 0x01);
  EXPECT_EQ(r.data[1023], 0x01);
  EXPECT_EQ(r.data[1024], 0x02);
  EXPECT_EQ(r.data[2047], 0x02);
  EXPECT_EQ(r.data[2048], 0x03);
  EXPECT_EQ(r.data[2499], 0x03);
}

TEST(EfsReader, OffsetOverflowFailsClosed) {
  // A misbehaving client claims more bytes_read than it was actually
  // asked for - must be rejected outright, never trusted into the
  // offset/remaining accounting (remaining -= bytes_read would
  // otherwise wrap around, since remaining is unsigned).
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  EfsReadResult lying_read =
      MakeRead(5, 0, std::vector<uint8_t>(1024, 0xAB));
  lying_read.bytes_read = 5000;  // > the 1024-byte chunk actually requested.
  c.read_responses = {lying_read};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsReader, OversizedFileRejected) {
  FakeEfsClient c;
  c.stat_response = MakeStat(kMaxFileSizeBytes + 1);

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.open_calls, 0);  // rejected before ever opening.
}

TEST(EfsReader, ReadFailureStillClosesHandle) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  // read_responses left empty - the fake's own "ran off script" path
  // (see FakeEfsClient::Read()) returns ok=false.

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsReader, MalformedReadStillClosesHandle) {
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  EfsReadResult malformed;
  malformed.ok = false;  // structurally malformed at the wire layer.
  c.read_responses = {malformed};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsReader, TimeoutPoisonsSessionAndClosesBestEffort) {
  // At the IEfsClient boundary, a real transport timeout surfaces as
  // ok=false (EfsRealClient returns a default-constructed, ok=false
  // result for any non-kOk TransportStatus, including kTimeout - see
  // efs_real_client.cpp). This reader cannot itself un-poison the
  // underlying DiagTransport session (that is session-level state this
  // reader has no access to - see diag_transport.h's
  // SessionState::kDesynchronized), but it must still attempt a
  // best-effort Close() on its own EFS-level file handle rather than
  // leaking it.
  FakeEfsClient c;
  c.stat_response = MakeStat(1024);
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  EfsReadResult timeout_read;
  timeout_read.ok = false;
  c.read_responses = {timeout_read};

  auto r = ReadWholeFile(&c, "/a");
  EXPECT_EQ(r.state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
  EXPECT_EQ(c.read_calls, 1);  // no retry.
}

TEST(EfsReader, EmptyFileCloseFailureIsReadFailure) {
  FakeEfsClient c;
  c.stat_response = MakeStat(0);
  c.open_response = MakeOpen();
  c.close_response = MakeClose(5);
  EXPECT_EQ(ReadWholeFile(&c, "/a").state, EfsReadFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}
