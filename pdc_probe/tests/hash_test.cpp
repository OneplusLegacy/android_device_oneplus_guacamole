#include "hash.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_pdc;

namespace {
std::string ToHex(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0xF]);
  }
  return out;
}
}  // namespace

// Standard FIPS 180-4 test vectors, generated via this host's own
// sha1sum/sha256sum (GNU coreutils) rather than hand-transcribed, to
// remove any risk of a typo'd expected value.

TEST(Sha1, EmptyString) {
  auto d = Sha1(nullptr, 0);
  EXPECT_EQ(ToHex(d.data(), d.size()), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(Sha1, Abc) {
  const char* s = "abc";
  auto d = Sha1(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1, LongerMultiBlockMessage) {
  // Exercises the multi-chunk path (FIPS 180-4's standard 448-bit vector).
  const char* s =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  auto d = Sha1(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha256, EmptyString) {
  auto d = Sha256(nullptr, 0);
  EXPECT_EQ(ToHex(d.data(), d.size()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
  const char* s = "abc";
  auto d = Sha256(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, LongerMultiBlockMessage) {
  const char* s =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  auto d = Sha256(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
