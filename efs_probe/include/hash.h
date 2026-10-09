// guacamole_efs_probe - minimal, self-contained SHA-256.
//
// Reused verbatim (algorithm unchanged, namespace adapted) from
// guacamole_pdc_probe's own hash.h/.cpp, which was verified against the
// standard FIPS 180-4 empty-string/"abc" test vectors
// (pdc_probe/tests/hash_test.cpp) - not re-derived here, just copied
// into this separate Soong module so efs_probe doesn't depend on
// pdc_probe's build target. Used for snapshot-mcfg-readonly's per-file
// digest output and the Cycle-2-prep transaction/rollback framework's
// preimage/replacement/rollback digests - see efs_transaction.h.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace guacamole_efs {

std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t len);

}  // namespace guacamole_efs
