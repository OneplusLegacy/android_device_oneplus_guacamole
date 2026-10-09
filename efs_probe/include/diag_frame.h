#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace guacamole_efs {

// Standard CRC-16/X-25 (a.k.a. CRC-16/IBM-SDLC / the HDLC FCS): poly
// 0x8408 (reflected form of 0x1021), init 0xFFFF, refin=true,
// refout=true, xorout=0xFFFF. Self-verified against the well-known
// public check value for this exact parameter set - see
// tests/diag_frame_test.cpp's Crc16X25.KnownCheckValue
// (Crc16X25("123456789") == 0x906E, the standard reveng-catalogue
// check value for CRC-16/X-25). ALWAYS computed over the UNESCAPED
// (de-stuffed) logical payload - never over raw/stuffed bytes.
uint16_t Crc16X25(const uint8_t* data, size_t len);

struct DiagFrameResult {
  // True only for verified framing, or explicit test-mode unframed input.
  bool ok = false;
  bool had_hdlc_trailer = false;
  bool crc_valid = false;  // only meaningful when had_hdlc_trailer is true.
  uint16_t crc_expected = 0;
  uint16_t crc_received = 0;
  // HDLC byte-stuffing metadata - only meaningful when
  // had_hdlc_trailer is true. escape_count counts `0x7d 0x5d`/
  // `0x7d 0x5e` PAIRS actually decoded (0 for the two real responses
  // observed before the third; 1 for that third real READDIR
  // response's escaped CRC byte).
  bool escape_present = false;
  int escape_count = 0;
  // Length of the body AFTER unescaping but BEFORE splitting off the
  // trailing 2-byte CRC (i.e. logical.size() + 2 when ok). Only
  // meaningful when had_hdlc_trailer is true - lets a caller report
  // exactly how much the unescape step changed the byte count.
  size_t unescaped_len = 0;
  std::vector<uint8_t> raw;       // exactly what was passed in, untouched.
  std::vector<uint8_t> logical;   // de-framed, unescaped, CRC-verified
                                    // payload - valid only if `ok`.
};

// H41 is strict by default. Raw logical packets require an explicit test mode.
enum class DiagFrameMode { kH41Framed, kTestAllowUnframed };
DiagFrameResult NormalizeDiagFrame(const std::vector<uint8_t>& raw,
    DiagFrameMode mode = DiagFrameMode::kH41Framed);

}  // namespace guacamole_efs
