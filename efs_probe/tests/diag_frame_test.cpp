#include "diag_frame.h"

#include "efs_dir_lister.h"
#include "efs_wire.h"

#include <gtest/gtest.h>

using namespace guacamole_efs;

TEST(Crc16X25, KnownCheckValue) {
  // The standard reveng-catalogue check value for CRC-16/X-25 (poly
  // 0x1021 normal / 0x8408 reflected, init 0xFFFF, refin/refout=true,
  // xorout 0xFFFF) over the ASCII bytes "123456789".
  const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(Crc16X25(data, sizeof(data)), 0x906E);
}

TEST(Crc16X25, EmptyInput) {
  EXPECT_EQ(Crc16X25(nullptr, 0), 0x0000);  // init 0xFFFF ^ xorout 0xFFFF.
}

// --- Two independent H41_DEVICE_CONFIRMED real-device response
// fixtures (2026-09-23, first real snapshot-mcfg-readonly run) - see
// diag_frame.h's header comment for the full evidence. ---

TEST(DiagFrame, H41RealStatFrameCrcValid) {
  // Real raw callback response to a STAT request for
  // /nv/item_files/mcfg.
  std::vector<uint8_t> raw = {
      0x4b, 0x13, 0x0f, 0x00,  // echo header.
      0x00, 0x00, 0x00, 0x00,  // diag_errno = 0.
      0xff, 0x41, 0x00, 0x00,  // mode = 0x000041ff.
      0x24, 0x00, 0x00, 0x00,  // size = 36.
      0x02, 0x00, 0x00, 0x00,  // nlink = 2.
      0x80, 0x3d, 0xd5, 0x12,  // atime.
      0x80, 0x3d, 0xd5, 0x12,  // mtime.
      0x80, 0x3d, 0xd5, 0x12,  // ctime.
      0x56, 0x67, 0x7e,        // CRC16-X25 (LE) + HDLC terminator.
  };
  ASSERT_EQ(raw.size(), 35u);

  auto r = NormalizeDiagFrame(raw);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_EQ(r.crc_expected, 0x6756);
  EXPECT_EQ(r.crc_received, 0x6756);
}

TEST(DiagFrame, H41RealStatFrameStripsTrailer) {
  std::vector<uint8_t> raw = {
      0x4b, 0x13, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x41, 0x00,
      0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x80, 0x3d,
      0xd5, 0x12, 0x80, 0x3d, 0xd5, 0x12, 0x80, 0x3d, 0xd5, 0x12, 0x56,
      0x67, 0x7e,
  };
  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.logical.size(), 32u);
  std::vector<uint8_t> expected_logical(raw.begin(), raw.end() - 3);
  EXPECT_EQ(r.logical, expected_logical);
}

TEST(DiagFrame, H41RealStatLogicalResponseParses) {
  std::vector<uint8_t> raw = {
      0x4b, 0x13, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x41, 0x00,
      0x00, 0x24, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x80, 0x3d,
      0xd5, 0x12, 0x80, 0x3d, 0xd5, 0x12, 0x80, 0x3d, 0xd5, 0x12, 0x56,
      0x67, 0x7e,
  };
  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  auto stat = ParseStatResponse(r.logical.data(), r.logical.size());
  ASSERT_TRUE(stat.ok);
  EXPECT_EQ(stat.diag_errno, 0);
  EXPECT_EQ(stat.mode, 0x000041ff);
  EXPECT_EQ(stat.size, 36);
  EXPECT_EQ(stat.nlink, 2);
}

TEST(DiagFrame, H41RealCallbackSmokeFrameCrcValid) {
  // Real raw callback response from the earlier callback-smoke run
  // (H.41's own known-good non-EFS2 loopback packet) - a SECOND,
  // independent real-device confirmation of the same framing.
  std::vector<uint8_t> raw = {
      0x4b, 0x12, 0x29, 0x00, 0x90, 0xbe, 0x47, 0xe9,
      0x8a, 0x96, 0x12, 0x01, 0x01, 0x02, 0x03, 0x00,
      0x0c, 0x30, 0x7e,  // CRC16-X25 (LE) + HDLC terminator.
  };
  auto r = NormalizeDiagFrame(raw);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_EQ(r.crc_expected, 0x300c);
  EXPECT_EQ(r.crc_received, 0x300c);
  EXPECT_EQ(r.logical.size(), 16u);
}

// --- Negative / policy tests (2026-09-23 preflight requirement). ---

TEST(DiagFrame, ValidTrailerAccepted) {
  std::vector<uint8_t> payload = {0x4b, 0x13, 0x0f, 0x00, 0xAA, 0xBB};
  uint16_t crc = Crc16X25(payload.data(), payload.size());
  std::vector<uint8_t> raw = payload;
  raw.push_back(static_cast<uint8_t>(crc & 0xFF));
  raw.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
  raw.push_back(0x7e);

  auto r = NormalizeDiagFrame(raw);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_EQ(r.logical, payload);
}

TEST(DiagFrame, WrongCrcRejected) {
  std::vector<uint8_t> payload = {0x4b, 0x13, 0x0f, 0x00, 0xAA, 0xBB};
  std::vector<uint8_t> raw = payload;
  raw.push_back(0x00);  // deliberately wrong CRC bytes.
  raw.push_back(0x00);
  raw.push_back(0x7e);

  auto r = NormalizeDiagFrame(raw);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_FALSE(r.crc_valid);
  EXPECT_TRUE(r.logical.empty());
}

TEST(DiagFrame, MissingTerminatorHandledAccordingToExplicitPolicy) {
  // No trailing 0x7e at all - treated as UNFRAMED, not an error, per
  // this file's documented policy (generic host-shim/fake
  // compatibility - every real H.41 response observed so far HAS been
  // framed; see diag_frame.h).
  std::vector<uint8_t> raw = {0x4b, 0x13, 0x0f, 0x00, 0xAA, 0xBB};
  auto r = NormalizeDiagFrame(raw, DiagFrameMode::kTestAllowUnframed);
  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(r.had_hdlc_trailer);
  EXPECT_EQ(r.logical, raw);
}

TEST(DiagFrame, TooShortFramedPacketRejected) {
  // Ends in 0x7e (so it LOOKS framed) but has fewer than 3 bytes total
  // - not enough room for a 2-byte CRC before the terminator.
  std::vector<uint8_t> raw = {0x7e};
  auto r = NormalizeDiagFrame(raw);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
}

TEST(DiagFrame, EmptyInputHandledAccordingToExplicitPolicy) {
  auto r = NormalizeDiagFrame({}, DiagFrameMode::kTestAllowUnframed);
  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.logical.empty());
}

TEST(DiagFrame, DanglingEscapeRejected) {
  // A 0x7d as the very last byte before the terminator, with nothing
  // to pair it with.
  std::vector<uint8_t> raw = {0x4b, 0x13, 0x0f, 0x00, 0x7d, 0x7e};
  auto r = NormalizeDiagFrame(raw);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.escape_present);
}

TEST(DiagFrame, UnrecognizedEscapeSequenceRejected) {
  // 0x7d followed by something other than 0x5d/0x5e - not a sequence
  // this project has any evidence for; never guess, fail closed.
  std::vector<uint8_t> raw = {0x4b, 0x13, 0x0f, 0x00, 0x7d, 0xAA, 0x7e};
  auto r = NormalizeDiagFrame(raw);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(r.escape_present);
}

// --- 2026-09-23 second preflight finding: a real READDIR response
// (seq=3, enumerating /nv/item_files/mcfg) had an HDLC-STUFFED CRC
// byte (0x7d escaped as 0x7d 0x5d) - the earlier revision of this file
// split the raw trailing 3 bytes into crc_lo/crc_hi/terminator BEFORE
// unescaping, misreading `5d 35` as the CRC and reporting a spurious
// FRAME_INTEGRITY_FAILED on a perfectly valid packet. Fixed by
// unescaping the WHOLE stuffed region (payload AND CRC) before ever
// splitting off the CRC bytes - see diag_frame.h/.cpp. ---

namespace {
std::vector<uint8_t> HexToBytes(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      return (c | 0x20) - 'a' + 10;
    };
    out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return out;
}

// The exact real raw bytes for a real READDIR (seq=3) response
// enumerating /nv/item_files/mcfg, whose entry is
// "mcfg_def_config_hw_version". Its trailing `7d 5d 35 7e` is an
// HDLC-stuffed CRC (`7d 5d` -> literal `7d`, so the real little-endian
// CRC is `7d 35` = 0x357d), not raw crc_lo/crc_hi/terminator.
constexpr const char* kRealReadDirSeq3Hex =
    "4b130c000100000003000000000000000f000000ffe1000004000000000000000000"
    "0000000000006d6366675f6465665f636f6e6669675f68775f76657273696f6e007d"
    "5d357e";
}  // namespace

TEST(DiagFrame, H41RealReadDirEscapedCrcAccepted) {
  auto raw = HexToBytes(kRealReadDirSeq3Hex);
  ASSERT_EQ(raw.size(), 71u);

  auto r = NormalizeDiagFrame(raw);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.escape_count, 1);
  EXPECT_EQ(r.unescaped_len, 69u);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_EQ(r.crc_expected, 0x357d);
  EXPECT_EQ(r.crc_received, 0x357d);
  EXPECT_EQ(r.logical.size(), 67u);
}

TEST(DiagFrame, H41RealReadDirUnescapesCrcBeforeValidation) {
  // Directly confirms the ORDERING fix: computing CRC over the
  // UNESCAPED payload (67 bytes) matches the UNESCAPED trailing CRC
  // (0x357d) - NOT over any raw/stuffed slice of the original 71
  // bytes, which is what the earlier, now-corrected revision did.
  auto raw = HexToBytes(kRealReadDirSeq3Hex);
  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(Crc16X25(r.logical.data(), r.logical.size()), r.crc_received);
}

TEST(DiagFrame, H41RealReadDirLogicalPayloadParses) {
  auto raw = HexToBytes(kRealReadDirSeq3Hex);
  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);

  auto rd = ParseReadDirResponse(r.logical.data(), r.logical.size());
  ASSERT_TRUE(rd.ok);
  EXPECT_EQ(rd.dirp, 1u);
  EXPECT_EQ(rd.seqno, 3);
  EXPECT_EQ(rd.diag_errno, 0);
  EXPECT_EQ(rd.name, "mcfg_def_config_hw_version");
}

// --- Payload-interior escape round-trip tests (item 4 of the
// 2026-09-23 second preflight review): the same unescape logic must
// correctly restore a literal 0x7d/0x7e that appears WITHIN the
// logical payload itself, not just within the CRC bytes. ---

namespace {
std::vector<uint8_t> StuffBytes(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  for (uint8_t b : in) {
    if (b == 0x7d) {
      out.push_back(0x7d);
      out.push_back(0x5d);
    } else if (b == 0x7e) {
      out.push_back(0x7d);
      out.push_back(0x5e);
    } else {
      out.push_back(b);
    }
  }
  return out;
}

// Encodes `payload` exactly the way real H.41 framing is now confirmed
// to work (see diag_frame.h): append the little-endian CRC, THEN
// stuff the whole payload+CRC region, THEN append the terminator.
std::vector<uint8_t> EncodeFrame(std::vector<uint8_t> payload) {
  uint16_t crc = Crc16X25(payload.data(), payload.size());
  payload.push_back(static_cast<uint8_t>(crc & 0xFF));
  payload.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
  auto stuffed = StuffBytes(payload);
  stuffed.push_back(0x7e);
  return stuffed;
}
}  // namespace

TEST(DiagFrame, Escaped7dInPayloadRestored) {
  std::vector<uint8_t> payload = {0x4b, 0x13, 0x0c, 0x00, 0x7d, 0xAA};
  auto raw = EncodeFrame(payload);

  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.logical, payload);
}

TEST(DiagFrame, Escaped7eInPayloadRestored) {
  std::vector<uint8_t> payload = {0x4b, 0x13, 0x0c, 0x00, 0x7e, 0xBB};
  auto raw = EncodeFrame(payload);

  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.logical, payload);
}

TEST(DiagFrame, MultipleEscapesRestored) {
  std::vector<uint8_t> payload = {0x4b, 0x13, 0x0c, 0x00, 0x7d,
                                   0x7e, 0x7d, 0x01, 0x7e, 0x02};
  auto raw = EncodeFrame(payload);

  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.escape_count, 4);  // 4 literal 0x7d/0x7e bytes in payload.
  EXPECT_EQ(r.logical, payload);
}

TEST(DiagFrame, EscapedCrcByteRestored) {
  // The real READDIR seq=3 fixture IS this case: its CRC's low byte
  // (0x7d) needed escaping, not any payload byte - the escape is in
  // the tail region, confirmed by exact position below.
  auto raw = HexToBytes(kRealReadDirSeq3Hex);
  // The stuffed CRC+terminator tail is `7d 5d 35 7e` - the 0x7d escape
  // byte is 4 bytes from the end, not adjacent to any payload byte.
  ASSERT_EQ(raw[raw.size() - 4], 0x7d);
  ASSERT_EQ(raw[raw.size() - 3], 0x5d);

  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.escape_count, 1);
  EXPECT_EQ(r.crc_received, 0x357d);
}

// --- 2026-09-23 third real-device finding: the real EOD signal for
// /nv/item_files/mcfg is a zero-metadata READDIR sentinel entry
// (diag_errno=0, every field zeroed, empty name), not the
// kEfsErrDirEntryNotFound errno path - see efs_dir_lister.h's header
// comment. This is the exact real raw seq=35 response, ties framing
// all the way through to the EOD semantic in one test. ---

TEST(DiagFrame, H41RealReadDirSeq35ZeroSentinelFrameValid) {
  auto raw = HexToBytes(
      "4b130c000200000023000000000000000000000000000000000000000000000000"
      "000000000000000002d47e");
  ASSERT_EQ(raw.size(), 44u);

  auto r = NormalizeDiagFrame(raw);
  ASSERT_TRUE(r.ok);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_FALSE(r.escape_present);  // no escape needed in this fixture.
  EXPECT_EQ(r.logical.size(), 41u);

  auto rd = ParseReadDirResponse(r.logical.data(), r.logical.size());
  ASSERT_TRUE(rd.ok);
  EXPECT_EQ(rd.dirp, 2u);
  EXPECT_EQ(rd.seqno, 35);
  EXPECT_EQ(rd.diag_errno, 0);
  EXPECT_EQ(rd.entry_type, 0);
  EXPECT_EQ(rd.mode, 0);
  EXPECT_EQ(rd.size, 0);
  EXPECT_TRUE(rd.name.empty());
  EXPECT_TRUE(IsZeroSentinelEndOfDirectory(rd));
}

TEST(DiagFrame, H41ModeRejectsMissingTerminatorAndEmptyInput) {
  EXPECT_FALSE(NormalizeDiagFrame({0x4b, 0x13, 0x0f, 0x00}).ok);
  EXPECT_FALSE(NormalizeDiagFrame({}).ok);
}
