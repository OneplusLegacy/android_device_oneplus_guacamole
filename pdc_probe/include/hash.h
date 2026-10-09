// guacamole_pdc_probe - minimal, self-contained SHA-1 / SHA-256.
//
// WHY IMPLEMENTED HERE INSTEAD OF LINKING libcrypto: this probe needs
// exactly two things - the PDC Load Config "Id" field, which is
// CONFIRMED (see research_pdc_load_config_id_scheme.md - both from
// libqmi's public reference client `qmicli-pdc.c`, and empirically
// verified against this exact real device's actual Common-Commercial
// config ID) to be the SHA-1 digest of the whole config file's bytes;
// and a SHA-256 identity check on the stock SKT MBN before touching it
// at all (see known_profiles.h). Pulling in a full crypto library for
// two hash functions is unnecessary weight for a single-purpose CLI
// tool; these are the standard, unmodified algorithms (FIPS 180-4),
// verified against the standard empty-string/"abc" test vectors in
// hash_test.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace guacamole_pdc {

std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t len);
std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t len);

}  // namespace guacamole_pdc
