#include "hash.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_efs;

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

// Standard FIPS 180-4 test vectors, independently regenerated via this
// host's own sha256sum (GNU coreutils) - matches guacamole_pdc_probe's
// own hash_test.cpp vectors exactly, cross-checked again here rather
// than assumed identical.

TEST(Sha256, EmptyString) {
  auto d = Sha256(nullptr, 0);
  EXPECT_EQ(ToHex(d.data(), d.size()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
  const char* s = "abc";
  auto d = Sha256(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, LongerMultiBlockMessage) {
  const char* s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  auto d = Sha256(reinterpret_cast<const uint8_t*>(s), strlen(s));
  EXPECT_EQ(ToHex(d.data(), d.size()),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
