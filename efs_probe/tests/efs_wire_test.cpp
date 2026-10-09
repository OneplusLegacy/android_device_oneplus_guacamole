#include "efs_wire.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_efs;

// --- Wire encoding: exact bytes, matching forth32/qtools efsio.c's
// send_efs_cmd()/efs_*() functions byte-for-byte (see efs_wire.h's file
// header for the full provenance). ---

TEST(EfsWireEncoding, StatRequestExactBytes) {
  auto req = BuildStatRequest("/a");
  std::vector<uint8_t> expected = {0x4B, 0x13, 0x0F, 0x00, '/', 'a', 0x00};
  EXPECT_EQ(req, expected);
}

TEST(EfsWireEncoding, OpenReadOnlyRequestExactBytes) {
  auto req = BuildOpenReadOnlyRequest("/a", kOflagReadOnly);
  ASSERT_EQ(req.size(), 4u + 4u + 4u + 100u);
  std::vector<uint8_t> header = {0x4B, 0x13, 0x02, 0x00};
  EXPECT_TRUE(std::equal(header.begin(), header.end(), req.begin()));
  // oflag (4 bytes LE) = 0
  EXPECT_EQ((std::vector<uint8_t>(req.begin() + 4, req.begin() + 8)),
            (std::vector<uint8_t>{0, 0, 0, 0}));
  // mode (4 bytes LE) = 0
  EXPECT_EQ((std::vector<uint8_t>(req.begin() + 8, req.begin() + 12)),
            (std::vector<uint8_t>{0, 0, 0, 0}));
  // name[100]: "/a" + NUL-padding
  EXPECT_EQ(req[12], '/');
  EXPECT_EQ(req[13], 'a');
  for (size_t i = 14; i < req.size(); i++) {
    EXPECT_EQ(req[i], 0) << "byte " << i;
  }
}

TEST(EfsWireEncoding, ReadRequestExactBytes) {
  auto req = BuildReadRequest(/*fd=*/5, /*nbyte=*/10, /*offset=*/20);
  std::vector<uint8_t> expected = {
      0x4B, 0x13, 0x04, 0x00,        // header, cmd=4 (READ)
      5, 0, 0, 0,                    // fd = 5
      10, 0, 0, 0,                   // nbyte = 10
      20, 0, 0, 0,                   // offset = 20
  };
  EXPECT_EQ(req, expected);
}

TEST(EfsWireEncoding, CloseRequestExactBytes) {
  auto req = BuildCloseRequest(/*fd=*/7);
  std::vector<uint8_t> expected = {0x4B, 0x13, 0x03, 0x00, 7, 0, 0, 0};
  EXPECT_EQ(req, expected);
}

TEST(EfsWireEncoding, OpenDirRequestExactBytes) {
  auto req = BuildOpenDirRequest("/a");
  std::vector<uint8_t> expected = {0x4B, 0x13, 0x0B, 0x00, '/', 'a', 0x00};
  EXPECT_EQ(req, expected);
}

TEST(EfsWireEncoding, ReadDirRequestExactBytes) {
  auto req = BuildReadDirRequest(/*dirp=*/100, /*seqno=*/3);
  std::vector<uint8_t> expected = {
      0x4B, 0x13, 0x0C, 0x00,  // header, cmd=12 (READDIR)
      100, 0, 0, 0,            // dirp = 100
      3, 0, 0, 0,              // seqno = 3
  };
  EXPECT_EQ(req, expected);
}

TEST(EfsWireEncoding, CloseDirRequestExactBytes) {
  auto req = BuildCloseDirRequest(/*dirp=*/100);
  std::vector<uint8_t> expected = {0x4B, 0x13, 0x0D, 0x00, 100, 0, 0, 0};
  EXPECT_EQ(req, expected);
}

// --- Safety: the request-construction layer itself refuses anything but
// a clean read-only open, and (via NoBuildFunctionProducesWriteCommandCode)
// this file's Build* surface never emits a mutating command code. ---

TEST(EfsWireSafety, OpenWriteOnlyRejectedBeforeTransport) {
  EXPECT_TRUE(BuildOpenReadOnlyRequest("/a", /*O_WRONLY=*/1).empty());
}

TEST(EfsWireSafety, OpenReadWriteRejectedBeforeTransport) {
  EXPECT_TRUE(BuildOpenReadOnlyRequest("/a", /*O_RDWR=*/2).empty());
}

TEST(EfsWireSafety, OpenCreateRejectedBeforeTransport) {
  EXPECT_TRUE(BuildOpenReadOnlyRequest("/a", /*O_CREAT=*/0100).empty());
}

TEST(EfsWireSafety, OpenTruncateRejectedBeforeTransport) {
  EXPECT_TRUE(BuildOpenReadOnlyRequest("/a", /*O_TRUNC=*/01000).empty());
}

TEST(EfsWireSafety, OpenPathTooLongRejected) {
  std::string long_path(200, 'x');
  EXPECT_TRUE(BuildOpenReadOnlyRequest(long_path, kOflagReadOnly).empty());
}

TEST(EfsWireSafety, StatPathTooLongRejected) {
  std::string long_path(300, 'x');
  EXPECT_TRUE(BuildStatRequest(long_path).empty());
}

TEST(EfsWireSafety, EmptyPathRejected) {
  EXPECT_TRUE(BuildStatRequest("").empty());
  EXPECT_TRUE(BuildOpenDirRequest("").empty());
  EXPECT_TRUE(BuildOpenReadOnlyRequest("", kOflagReadOnly).empty());
}

TEST(EfsWireSafety, ReadRequestOverCapRejected) {
  EXPECT_TRUE(BuildReadRequest(1, kMaxReadRequestBytes + 1, 0).empty());
}

TEST(EfsWireSafety, ReadRequestZeroBytesRejected) {
  EXPECT_TRUE(BuildReadRequest(1, 0, 0).empty());
}

// This file's public API only exposes 7 Build* functions - one per
// read-only EFS2 command. This test is a lightweight automated proxy
// for the "no write-capable code path exists" audit requirement: it
// builds one request per exposed function and asserts none of them ever
// carries a known-mutating command code (WRITE=5, UNLINK=8, MKDIR=9,
// RMDIR=10, RENAME=14, CHMOD=18, CHOWN=30, PUT=38, TRUNCATE=40,
// FTRUNCATE=41, SHRED=45 - the full public qtools enum) in byte 2 of
// its header. The REAL guarantee is structural (this .cpp file contains
// no code that could produce those bytes at all - verifiable by
// inspection/grep, see the completion report's audit section) - this
// test is a belt-and-suspenders runtime check on top of that.
TEST(EfsWireSafety, NoBuildFunctionProducesAMutatingCommandCode) {
  const std::vector<uint8_t> mutating_codes = {5,  8,  9,  10, 14, 18,
                                                30, 38, 39, 40, 41, 45};
  std::vector<std::vector<uint8_t>> requests = {
      BuildStatRequest("/a"),
      BuildOpenDirRequest("/a"),
      BuildCloseDirRequest(1),
      BuildReadDirRequest(1, 0),
      BuildOpenReadOnlyRequest("/a", kOflagReadOnly),
      BuildReadRequest(1, 10, 0),
      BuildCloseRequest(1),
  };
  for (const auto& req : requests) {
    ASSERT_GE(req.size(), 4u);
    for (uint8_t bad : mutating_codes) {
      EXPECT_NE(req[2], bad) << "a Build* function emitted mutating cmd "
                              << static_cast<int>(bad);
    }
  }
}

// --- Parser: malformed/mismatched responses must be rejected, never
// best-effort accepted. ---

namespace {
std::vector<uint8_t> MakeStatResponse(int32_t diag_errno, int32_t mode,
                                       int32_t size, int32_t nlink,
                                       int32_t atime, int32_t mtime,
                                       int32_t ctime) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x0F, 0x00};
  auto push = [&](int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    out.push_back(u & 0xFF);
    out.push_back((u >> 8) & 0xFF);
    out.push_back((u >> 16) & 0xFF);
    out.push_back((u >> 24) & 0xFF);
  };
  push(diag_errno);
  push(mode);
  push(size);
  push(nlink);
  push(atime);
  push(mtime);
  push(ctime);
  return out;
}

std::vector<uint8_t> MakeReadResponse(int32_t fd, uint32_t offset,
                                       int32_t bytes_read, int32_t diag_errno,
                                       const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x04, 0x00};
  auto push_i32 = [&](int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    out.push_back(u & 0xFF);
    out.push_back((u >> 8) & 0xFF);
    out.push_back((u >> 16) & 0xFF);
    out.push_back((u >> 24) & 0xFF);
  };
  push_i32(fd);
  push_i32(static_cast<int32_t>(offset));
  push_i32(bytes_read);
  push_i32(diag_errno);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}
}  // namespace

TEST(EfsWireParser, StatSuccessRoundtrip) {
  auto resp = MakeStatResponse(0, 0100644, 12345, 1, 111, 222, 333);
  auto r = ParseStatResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.diag_errno, 0);
  EXPECT_EQ(r.size, 12345);
}

TEST(EfsWireParser, StatShortResponseRejected) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3};  // way short.
  auto r = ParseStatResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, StatWrongCmdEchoRejected) {
  auto resp = MakeStatResponse(0, 0, 0, 0, 0, 0, 0);
  resp[2] = 0x02;  // echoes OPEN's cmd instead of STAT's - must reject.
  auto r = ParseStatResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, StatWrongMagicRejected) {
  auto resp = MakeStatResponse(0, 0, 0, 0, 0, 0, 0);
  resp[0] = 0x00;
  auto r = ParseStatResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, StatWrongSubsysRejected) {
  auto resp = MakeStatResponse(0, 0, 0, 0, 0, 0, 0);
  resp[1] = 0x3E;  // qtools' "alternate EFS" subsys - not what we sent.
  auto r = ParseStatResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadSuccessRoundtrip) {
  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  auto resp = MakeReadResponse(5, 20, 5, 0, data);
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/20, /*requested_nbyte=*/10);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.data, data);
}

TEST(EfsWireParser, ReadFdMismatchRejected) {
  std::vector<uint8_t> data = {1, 2, 3};
  auto resp = MakeReadResponse(99 /* wrong fd */, 0, 3, 0, data);
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/0, /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadOffsetMismatchRejected) {
  std::vector<uint8_t> data = {1, 2, 3};
  auto resp = MakeReadResponse(5, 999 /* wrong offset */, 3, 0, data);
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/0, /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadBytesLargerThanRequestedRejected) {
  std::vector<uint8_t> data(20, 0xAB);
  auto resp = MakeReadResponse(5, 0, 20, 0, data);
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/0,
                              /*requested_nbyte=*/10 /* asked for less */);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadTruncatedPayloadRejected) {
  // Claims bytes_read=10 but only 3 data bytes actually follow.
  std::vector<uint8_t> data = {1, 2, 3};
  auto resp = MakeReadResponse(5, 0, 10, 0, data);
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/0, /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadNegativeBytesRejected) {
  auto resp = MakeReadResponse(5, 0, -1, 0, {});
  auto r = ParseReadResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                              /*expected_offset=*/0, /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

namespace {
std::vector<uint8_t> MakeReadDirResponse(uint32_t dirp, int32_t seqno,
                                          int32_t errno_v, int32_t entry_type,
                                          int32_t mode, int32_t size,
                                          const std::string& name,
                                          bool omit_nul = false) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x0C, 0x00};
  auto push_u32 = [&](uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
  };
  auto push_i32 = [&](int32_t v) { push_u32(static_cast<uint32_t>(v)); };
  push_u32(dirp);
  push_i32(seqno);
  push_i32(errno_v);
  push_i32(entry_type);
  push_i32(mode);
  push_i32(size);
  push_i32(0);  // atime
  push_i32(0);  // mtime
  push_i32(0);  // ctime
  std::vector<uint8_t> name_field(100, 0);
  size_t copy_len = std::min(name.size(), omit_nul ? size_t(100) : size_t(99));
  std::memcpy(name_field.data(), name.data(), copy_len);
  if (omit_nul) {
    // Fill the entire 100 bytes with non-NUL content - no terminator
    // anywhere in the field.
    for (auto& b : name_field) {
      if (b == 0) b = 'x';
    }
  }
  out.insert(out.end(), name_field.begin(), name_field.end());
  return out;
}
}  // namespace

TEST(EfsWireParser, ReadDirSuccessRoundtrip) {
  auto resp = MakeReadDirResponse(1, 0, 0, 0, 0100644, 24508,
                                   "mcfg_sel_db.xml");
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.name, "mcfg_sel_db.xml");
  EXPECT_EQ(r.size, 24508);
}

TEST(EfsWireParser, DirectoryEntryMalformedRejected) {
  auto resp = MakeReadDirResponse(1, 0, 0, 0, 0, 0, "no-terminator",
                                   /*omit_nul=*/true);
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, DirectoryEntryWrongLengthRejected) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0C, 0x00, 1, 2, 3};  // way short.
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

// --- 2026-09-23 preflight hardening: ParseReadDirResponse must accept
// BOTH public reference layouts (qtools' fixed name[100], OpenPST's
// variable-length tail) and a minimal error response, while still
// rejecting genuinely malformed success responses. ---

namespace {
std::vector<uint8_t> MakeReadDirErrorResponse(uint32_t dirp, int32_t seqno,
                                               int32_t errno_v) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x0C, 0x00};
  auto push_u32 = [&](uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
  };
  auto push_i32 = [&](int32_t v) { push_u32(static_cast<uint32_t>(v)); };
  push_u32(dirp);
  push_i32(seqno);
  push_i32(errno_v);
  return out;  // exactly 12 payload bytes - no metadata/name at all.
}

// Builds a SUCCESS (errno=0) response with the 36-byte fixed metadata
// followed by exactly `name.size()+1` bytes of tail (the name plus a
// single NUL) - no padding to 100 bytes, matching OpenPST's
// variable-length `name[1]` convention rather than qtools' fixed one.
std::vector<uint8_t> MakeReadDirResponseVariableName(
    uint32_t dirp, int32_t seqno, int32_t entry_type, int32_t mode,
    int32_t size, const std::string& name) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x0C, 0x00};
  auto push_u32 = [&](uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
  };
  auto push_i32 = [&](int32_t v) { push_u32(static_cast<uint32_t>(v)); };
  push_u32(dirp);
  push_i32(seqno);
  push_i32(0);  // diag_errno = 0 (success).
  push_i32(entry_type);
  push_i32(mode);
  push_i32(size);
  push_i32(0);  // atime
  push_i32(0);  // mtime
  push_i32(0);  // ctime
  out.insert(out.end(), name.begin(), name.end());
  out.push_back(0);  // single NUL terminator, no further padding.
  return out;
}
}  // namespace

TEST(EfsWireParser, ReadDirFixed100ByteNameAccepted) {
  auto resp = MakeReadDirResponse(1, 0, 0, 0, 0100644, 24508,
                                   "mcfg_sel_db.xml");  // fixed 100-byte tail.
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.diag_errno, 0);
  EXPECT_EQ(r.name, "mcfg_sel_db.xml");
}

TEST(EfsWireParser, ReadDirVariableLengthNameAccepted) {
  auto resp = MakeReadDirResponseVariableName(1, 0, 0, 0100644, 4, "cards");
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.diag_errno, 0);
  EXPECT_EQ(r.name, "cards");
}

TEST(EfsWireParser, ReadDirErrorMinimalResponseAccepted) {
  auto resp = MakeReadDirErrorResponse(1, 5, kEfsErrInvalidSequence);
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);  // structurally parsed, NOT operation success.
  EXPECT_EQ(r.dirp, 1u);
  EXPECT_EQ(r.seqno, 5);
  EXPECT_EQ(r.diag_errno, kEfsErrInvalidSequence);
}

TEST(EfsWireParser, ReadDirSuccessMissingNameRejected) {
  // 36 bytes of fixed metadata, errno=0, but zero bytes of name tail.
  auto full = MakeReadDirResponseVariableName(1, 0, 0, 0, 0, "");
  // MakeReadDirResponseVariableName always appends a NUL even for an
  // empty name - truncate that one byte off to get a true "no name
  // field at all" response.
  std::vector<uint8_t> resp(full.begin(), full.end() - 1);
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadDirSuccessNameWithoutNulRejected) {
  std::vector<uint8_t> resp = MakeReadDirResponseVariableName(
      1, 0, 0, 0, 0, "abc");
  resp.pop_back();  // drop the trailing NUL - "abc" with no terminator.
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadDirOversizedNameRejected) {
  // 36-byte metadata + 101 tail bytes (one more than the max recognized
  // name field of 100) - rejected regardless of content.
  auto resp = MakeReadDirResponseVariableName(1, 0, 0, 0, 0,
                                               std::string(100, 'a'));
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, ReadDirTruncatedMetadataRejected) {
  // 20 bytes total: past the 12-byte common header, but short of the
  // full 36-byte fixed metadata a success (errno=0) response requires.
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0C, 0x00,
                                1, 0, 0, 0,   // dirp
                                0, 0, 0, 0,   // seqno
                                0, 0, 0, 0,   // diag_errno = 0 (success)
                                0, 0, 0, 0};  // only 4 of the remaining
                                              // 24 metadata bytes present.
  auto r = ParseReadDirResponse(resp.data(), resp.size());
  EXPECT_FALSE(r.ok);
}

TEST(EfsWireParser, OpenSuccessRoundtrip) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x02, 0x00, 3, 0, 0, 0, 0, 0, 0, 0};
  auto r = ParseOpenResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.fd, 3);
  EXPECT_EQ(r.diag_errno, 0);
}

TEST(EfsWireParser, OpenDirSuccessRoundtrip) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0B, 0x00, 7, 0, 0, 0, 0, 0, 0, 0};
  auto r = ParseOpenDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.dirp, 7u);
}

TEST(EfsWireParser, CloseSuccessRoundtrip) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x03, 0x00, 0, 0, 0, 0};
  auto r = ParseCloseResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.errno_value, 0);
}

TEST(EfsWireParser, CloseDirSuccessRoundtrip) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0D, 0x00, 0, 0, 0, 0};
  auto r = ParseCloseDirResponse(resp.data(), resp.size());
  ASSERT_TRUE(r.ok);
}

TEST(EfsWireParser, EmptyResponseRejectedEverywhere) {
  EXPECT_FALSE(ParseStatResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseOpenResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseOpenDirResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseReadDirResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseCloseResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseCloseDirResponse(nullptr, 0).ok);
  EXPECT_FALSE(ParseReadResponse(nullptr, 0, 0, 0, 10).ok);
}
