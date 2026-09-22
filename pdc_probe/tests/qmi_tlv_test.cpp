#include "qmi_tlv.h"

#include <cstring>

#include <gtest/gtest.h>

using guacamole_pdc::AppendTlv;
using guacamole_pdc::AppendTlvU32;
using guacamole_pdc::AppendTlvU8PrefixedBytes;
using guacamole_pdc::DecodeOneTlv;
using guacamole_pdc::TlvAsU16;
using guacamole_pdc::TlvAsU32;
using guacamole_pdc::TlvAsU8PrefixedBytes;
using guacamole_pdc::TlvAsU8PrefixedString;

TEST(QmiTlv, EncodeDecodeU32Roundtrip) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(AppendTlvU32(&buf, 0x10, 0xDEADBEEF));
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  EXPECT_EQ(tlv->type, 0x10);
  EXPECT_EQ(tlv->length, 4);
  EXPECT_EQ(next, buf.size());
  auto v = TlvAsU32(*tlv);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 0xDEADBEEFu);
}

TEST(QmiTlv, DecodeEmptyBufferFails) {
  size_t next = 0;
  auto tlv = DecodeOneTlv(nullptr, 0, 0, &next);
  EXPECT_FALSE(tlv.has_value());
}

TEST(QmiTlv, DecodeTruncatedHeaderFails) {
  uint8_t buf[2] = {0x10, 0x00};  // only 2 of the required 3 header bytes
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf, sizeof(buf), 0, &next);
  EXPECT_FALSE(tlv.has_value());
}

TEST(QmiTlv, DecodeMalformedLengthOverrunsBufferFails) {
  // type=0x11, length=0xFFFF (LE), but no actual payload follows.
  uint8_t buf[3] = {0x11, 0xFF, 0xFF};
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf, sizeof(buf), 0, &next);
  EXPECT_FALSE(tlv.has_value())
      << "a TLV claiming more bytes than the buffer actually has must be "
         "rejected, not clamped";
}

TEST(QmiTlv, DecodeTruncatedResponseMidStreamFails) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(AppendTlvU32(&buf, 0x10, 42));
  ASSERT_TRUE(AppendTlvU32(&buf, 0x11, 99));
  buf.resize(buf.size() - 2);  // chop off the tail of the second TLV
  size_t offset = 0, next = 0;
  auto first = DecodeOneTlv(buf.data(), buf.size(), offset, &next);
  ASSERT_TRUE(first.has_value());
  offset = next;
  auto second = DecodeOneTlv(buf.data(), buf.size(), offset, &next);
  EXPECT_FALSE(second.has_value());
}

TEST(QmiTlv, U16WrongLengthRejected) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(AppendTlvU32(&buf, 0x01, 5));  // 4 bytes, not the 2 U16 wants
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  EXPECT_FALSE(TlvAsU16(*tlv).has_value());
}

TEST(QmiTlv, U8PrefixedBytesRoundtrip) {
  std::vector<uint8_t> buf;
  const uint8_t data[] = {1, 2, 3, 4, 5};
  ASSERT_TRUE(AppendTlvU8PrefixedBytes(&buf, 0x11, data, sizeof(data)));
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  auto bytes = TlvAsU8PrefixedBytes(*tlv, 100);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(bytes->size(), 5u);
  EXPECT_EQ((*bytes)[4], 5);
}

TEST(QmiTlv, U8PrefixedBytesOversizedRejectedByCap) {
  std::vector<uint8_t> buf;
  std::vector<uint8_t> data(200, 0xAB);
  ASSERT_TRUE(AppendTlvU8PrefixedBytes(&buf, 0x11, data.data(), data.size()));
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  // Wire-valid (declared len 200 fits inside the TLV), but our own trust
  // cap (say, 124 - PDC's real MAX_PDC_ID_LEN) must still reject it.
  auto bytes = TlvAsU8PrefixedBytes(*tlv, 124);
  EXPECT_FALSE(bytes.has_value());
}

TEST(QmiTlv, U8PrefixedBytesDeclaredLenExceedsTlvRejected) {
  // Hand-craft a TLV whose 1-byte size prefix (claiming 50) is larger
  // than the TLV's own outer length (only 10 bytes total value).
  std::vector<uint8_t> buf;
  uint8_t value[10] = {50, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  ASSERT_TRUE(AppendTlv(&buf, 0x11, value, sizeof(value)));
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  auto bytes = TlvAsU8PrefixedBytes(*tlv, 124);
  EXPECT_FALSE(bytes.has_value());
}

TEST(QmiTlv, U8PrefixedStringDoesNotRelyOnNulTermination) {
  std::vector<uint8_t> buf;
  // No NUL byte anywhere in the payload - the decoder must not read past
  // the declared length looking for one, and must not require one.
  const char* text = "Korea SW config";
  ASSERT_TRUE(AppendTlvU8PrefixedBytes(
      &buf, 0x12, reinterpret_cast<const uint8_t*>(text), strlen(text)));
  size_t next = 0;
  auto tlv = DecodeOneTlv(buf.data(), buf.size(), 0, &next);
  ASSERT_TRUE(tlv.has_value());
  auto s = TlvAsU8PrefixedString(*tlv, 512);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(*s, "Korea SW config");
}

TEST(QmiTlv, AppendRejectsOversized16BitLength) {
  std::vector<uint8_t> buf;
  std::vector<uint8_t> huge(70000, 0);
  EXPECT_FALSE(AppendTlv(&buf, 0x01, huge.data(), huge.size()));
}
