#include "known_profiles.h"

#include <gtest/gtest.h>

using namespace guacamole_pdc;

namespace {
std::array<uint8_t, 32> Sha256FromHex(const char* hex) {
  std::array<uint8_t, 32> out{};
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    return (c | 0x20) - 'a' + 10;
  };
  for (size_t i = 0; i < 32; i++) {
    out[i] = static_cast<uint8_t>((nibble(hex[i * 2]) << 4) |
                                   nibble(hex[i * 2 + 1]));
  }
  return out;
}
}  // namespace

// --- Cycle 2 Section H: KT/LGU profiles added 2026-09-24, real SHA-256
// values cross-checked against a fresh hash of the actual firmware
// files (see PHASE_D2_EFS2_WRITE_RESEARCH.md's Section C) -
// independently re-verified here against the exact same hex strings. ---

TEST(KnownProfiles, SktProfileExactValues) {
  const auto& p = GetSktProfile();
  EXPECT_STREQ(p.name, "skt");
  EXPECT_EQ(p.expected_size, 41172u);
  EXPECT_EQ(p.expected_sha256,
            Sha256FromHex("9ee13411bd223576b0bdd14823929ace3544f1f1c2eab57"
                           "a23a30038bc33805e"));
  EXPECT_NE(std::string(p.runtime_path).find("korea/skt"), std::string::npos);
}

TEST(KnownProfiles, KtProfileExactValues) {
  const auto& p = GetKtProfile();
  EXPECT_STREQ(p.name, "kt");
  EXPECT_EQ(p.expected_size, 39544u);
  EXPECT_EQ(p.expected_sha256,
            Sha256FromHex("0305b51ca7446db9bb16e78964f6c6520739281c228ff59"
                           "6987d23484b9cd101"));
  EXPECT_NE(std::string(p.runtime_path).find("korea/kt"), std::string::npos);
}

TEST(KnownProfiles, LguProfileExactValues) {
  const auto& p = GetLguProfile();
  EXPECT_STREQ(p.name, "lgu");
  EXPECT_EQ(p.expected_size, 41088u);
  EXPECT_EQ(p.expected_sha256,
            Sha256FromHex("c84bcf76cb626debee291d6ee9b87e83b7c686070efe541"
                           "c28d6ef46b84819b2"));
  EXPECT_NE(std::string(p.runtime_path).find("korea/lgu"), std::string::npos);
}

TEST(KnownProfiles, AllThreeProfilesHaveDistinctHashesAndPaths) {
  const auto& skt = GetSktProfile();
  const auto& kt = GetKtProfile();
  const auto& lgu = GetLguProfile();
  EXPECT_NE(skt.expected_sha256, kt.expected_sha256);
  EXPECT_NE(skt.expected_sha256, lgu.expected_sha256);
  EXPECT_NE(kt.expected_sha256, lgu.expected_sha256);
  EXPECT_STRNE(skt.runtime_path, kt.runtime_path);
  EXPECT_STRNE(skt.runtime_path, lgu.runtime_path);
  EXPECT_STRNE(kt.runtime_path, lgu.runtime_path);
}

TEST(KnownProfiles, GetProfileForCarrierDispatchesCorrectly) {
  EXPECT_EQ(GetProfileForCarrier(Carrier::kSkt), &GetSktProfile());
  EXPECT_EQ(GetProfileForCarrier(Carrier::kKt), &GetKtProfile());
  EXPECT_EQ(GetProfileForCarrier(Carrier::kLgu), &GetLguProfile());
}

TEST(KnownProfiles, CarrierToStringExactValues) {
  EXPECT_STREQ(CarrierToString(Carrier::kSkt), "SKT");
  EXPECT_STREQ(CarrierToString(Carrier::kKt), "KT");
  EXPECT_STREQ(CarrierToString(Carrier::kLgu), "LGU");
}

TEST(KnownProfiles, InvalidCarrierDoesNotFallbackToSkt) { EXPECT_EQ(GetProfileForCarrier(static_cast<Carrier>(99)), nullptr); }
