#include "efs_write_wire.h"

#include <algorithm>
#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_efs;

// --- Wire encoding: exact bytes, cross-checked against forth32/qtools'
// efs_write()/efs_open() (see efs_write_wire.h's file header for the
// exact 2026-09-24 fetch this is transcribed from). ---

TEST(EfsWriteWireEncoding, OpenWriteRequestExactBytes) {
  auto req = BuildOpenWriteRequest("/a");
  ASSERT_EQ(req.size(), 4u + 4u + 4u + 100u);
  std::vector<uint8_t> header = {0x4B, 0x13, 0x02, 0x00};  // OPEN=2, shared.
  EXPECT_TRUE(std::equal(header.begin(), header.end(), req.begin()));
  // oflag (4 bytes LE) = kOflagWriteTruncate = 0x241.
  EXPECT_EQ((std::vector<uint8_t>(req.begin() + 4, req.begin() + 8)),
            (std::vector<uint8_t>{0x41, 0x02, 0, 0}));
  // mode (4 bytes LE) = 0777 octal = 511 decimal = 0x1FF.
  EXPECT_EQ((std::vector<uint8_t>(req.begin() + 8, req.begin() + 12)),
            (std::vector<uint8_t>{0xFF, 0x01, 0, 0}));
  // name[100]: "/a" + NUL-padding.
  EXPECT_EQ(req[12], '/');
  EXPECT_EQ(req[13], 'a');
  for (size_t i = 14; i < req.size(); i++) {
    EXPECT_EQ(req[i], 0) << "byte " << i;
  }
}

TEST(EfsWriteWireEncoding, WriteRequestExactBytes) {
  std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
  auto req = BuildWriteRequest(/*fd=*/5, /*offset=*/20, data);
  std::vector<uint8_t> expected = {
      0x4B, 0x13, 0x05, 0x00,  // header, cmd=5 (WRITE).
      5, 0, 0, 0,              // fd = 5.
      20, 0, 0, 0,             // offset = 20.
      0xAA, 0xBB, 0xCC,        // data.
  };
  EXPECT_EQ(req, expected);
}

// --- Safety: the write-mode OPEN request has NO parameter that could
// ever select a different oflag - unlike the read path's
// BuildOpenReadOnlyRequest(path, oflag), which at least validates a
// caller-passed value, this function structurally cannot be asked to
// build anything but kOflagWriteTruncate. This test corroborates that
// by inspecting the produced bytes directly (belt-and-suspenders on
// top of the type-level guarantee - there is no oflag parameter to
// misuse in the first place). ---

TEST(EfsWriteWireSafety, OpenWriteAlwaysUsesFixedTruncateFlag) {
  auto req = BuildOpenWriteRequest("/nv/item_files/mcfg/mcfg_sel_db.xml");
  ASSERT_GE(req.size(), 8u);
  int32_t oflag;
  std::memcpy(&oflag, req.data() + 4, 4);
  EXPECT_EQ(oflag, kOflagWriteTruncate);
  EXPECT_EQ(kOflagWriteTruncate, 0x241);  // O_WRONLY|O_CREAT|O_TRUNC.
}

TEST(EfsWriteWireSafety, EmptyPathRejected) {
  EXPECT_TRUE(BuildOpenWriteRequest("").empty());
}

TEST(EfsWriteWireSafety, OpenWritePathTooLongRejected) {
  std::string long_path(200, 'x');
  EXPECT_TRUE(BuildOpenWriteRequest(long_path).empty());
}

TEST(EfsWriteWireSafety, WriteRequestZeroBytesRejected) {
  EXPECT_TRUE(BuildWriteRequest(1, 0, {}).empty());
}

TEST(EfsWriteWireSafety, WriteRequestOverCapRejected) {
  std::vector<uint8_t> data(kMaxWriteRequestBytes + 1, 0xAB);
  EXPECT_TRUE(BuildWriteRequest(1, 0, data).empty());
}

TEST(EfsWriteWireSafety, WriteRequestExactlyAtCapAccepted) {
  std::vector<uint8_t> data(kMaxWriteRequestBytes, 0xAB);
  EXPECT_FALSE(BuildWriteRequest(1, 0, data).empty());
}

// This file's public API only exposes 2 Build* functions
// (BuildOpenWriteRequest, BuildWriteRequest) - a lightweight automated
// proxy for the audit requirement that no OTHER mutating command code
// (UNLINK=8, MKDIR=9, RMDIR=10, RENAME=14, PUT=38, TRUNCATE=40, etc.)
// is ever emitted. The REAL guarantee is structural (this .cpp file
// contains no code that could produce those bytes at all).
TEST(EfsWriteWireSafety, OnlyOpenAndWriteCommandCodesEverProduced) {
  std::vector<uint8_t> allowed_codes = {0x02 /* OPEN, shared */,
                                         0x05 /* WRITE */};
  std::vector<std::vector<uint8_t>> requests = {
      BuildOpenWriteRequest("/a"),
      BuildWriteRequest(1, 0, {0x01}),
  };
  for (const auto& req : requests) {
    ASSERT_GE(req.size(), 4u);
    EXPECT_NE(std::find(allowed_codes.begin(), allowed_codes.end(), req[2]),
              allowed_codes.end())
        << "unexpected command code " << static_cast<int>(req[2]);
  }
}

// --- Parser: malformed/mismatched responses must be rejected. ---

namespace {
std::vector<uint8_t> MakeWriteResponse(int32_t fd, uint32_t offset,
                                        int32_t bytes_written,
                                        int32_t diag_errno) {
  std::vector<uint8_t> out = {0x4B, 0x13, 0x05, 0x00};
  auto push_i32 = [&](int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    out.push_back(u & 0xFF);
    out.push_back((u >> 8) & 0xFF);
    out.push_back((u >> 16) & 0xFF);
    out.push_back((u >> 24) & 0xFF);
  };
  push_i32(fd);
  push_i32(static_cast<int32_t>(offset));
  push_i32(bytes_written);
  push_i32(diag_errno);
  return out;
}
}  // namespace

TEST(EfsWriteWireParser, WriteSuccessRoundtrip) {
  auto resp = MakeWriteResponse(5, 20, 3, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/20, /*requested_nbyte=*/3);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.bytes_written, 3);
  EXPECT_EQ(r.diag_errno, 0);
}

TEST(EfsWriteWireParser, WriteFdMismatchRejected) {
  auto resp = MakeWriteResponse(99, 0, 3, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/0, /*requested_nbyte=*/3);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, WriteOffsetMismatchRejected) {
  auto resp = MakeWriteResponse(5, 999, 3, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/0, /*requested_nbyte=*/3);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, BytesWrittenLargerThanRequestedRejected) {
  auto resp = MakeWriteResponse(5, 0, 20, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/0,
                               /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, NegativeBytesWrittenRejected) {
  auto resp = MakeWriteResponse(5, 0, -1, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/0,
                               /*requested_nbyte=*/10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, ShortWriteAcceptedAsStructurallyValid) {
  // bytes_written < requested is STRUCTURALLY valid (a short write is
  // a real possibility the wire format allows for) - efs_writer.cpp's
  // orchestration layer is what decides a short write is a policy
  // failure (see its own ShortWriteRejected-style tests), not this
  // parser.
  auto resp = MakeWriteResponse(5, 0, 2, 0);
  auto r = ParseWriteResponse(resp.data(), resp.size(), /*expected_fd=*/5,
                               /*expected_offset=*/0,
                               /*requested_nbyte=*/10);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.bytes_written, 2);
}

TEST(EfsWriteWireParser, WriteWrongLengthRejected) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x05, 0x00, 1, 2, 3};  // way short.
  auto r = ParseWriteResponse(resp.data(), resp.size(), 0, 0, 10);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, WriteWrongCmdEchoRejected) {
  auto resp = MakeWriteResponse(5, 0, 3, 0);
  resp[2] = 0x04;  // echoes READ's cmd instead of WRITE's.
  auto r = ParseWriteResponse(resp.data(), resp.size(), 5, 0, 3);
  EXPECT_FALSE(r.ok);
}

TEST(EfsWriteWireParser, EmptyResponseRejected) {
  EXPECT_FALSE(ParseWriteResponse(nullptr, 0, 0, 0, 10).ok);
}

TEST(EfsWriteWireParser, NonzeroErrnoStillParsesStructurally) {
  // diag_errno != 0 is an EFS-level rejection, not a parse failure -
  // matches this project's own decode-vs-application-result separation
  // (see efs_wire.h's Parse* contract).
  auto resp = MakeWriteResponse(5, 0, 0, 5);
  auto r = ParseWriteResponse(resp.data(), resp.size(), 5, 0, 10);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.diag_errno, 5);
  EXPECT_EQ(r.bytes_written, 0);
}
