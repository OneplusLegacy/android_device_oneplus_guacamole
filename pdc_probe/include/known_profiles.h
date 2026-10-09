// guacamole_pdc_probe - internal known-profile table for Phase
// C0/Cycle 2 Section H. Deliberately the ONLY way to select a file to
// Load: the runtime path, expected size, and expected SHA-256 are
// compiled-in constants, never accepted from a CLI argument,
// environment variable, or any other external input. This is a direct
// requirement from the Phase C0 spec ("daemon/probe 외부에서 임의 파일
// 경로를 넘길 수 있게 만들지 않는다") - a debug CLI that could be
// pointed at an arbitrary file would defeat the whole point of the
// stock-firmware-only, byte-exact-verified design this project is
// built around. Cycle 2 Section H extends this to KT/LGU via the SAME
// discipline - real files, real hashes, real evidence, only ever
// selected through the `Carrier` enum below.
//
// VALUES: SKT computed directly from the real H.41 extraction this
// whole project's research is based on (see
// guacamole_efsd_design_report.md, guacamole_efsd_stock_modem_design.md)
// - confirmed via both this project's own `sha256sum`/`sha1sum` runs
// and a from-scratch reimplementation cross-check (hash.h/hash.cpp,
// verified against the same file in
// research_pdc_load_config_id_scheme.md's investigation). KT/LGU added
// 2026-09-24 (Cycle 2 Section H): located at
// `~/dumpyara/working/oneplus7profirmware/modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/{kt,lgu}/commerci/mcfg_sw.mbn`
// (this exact device's own prior firmware extraction), hashed directly
// - see PHASE_D2_EFS2_WRITE_RESEARCH.md's Section C for the full
// evidence, including the SHA-1 values (the actual PDC wire config ID,
// per the already-CONFIRMED SHA-1-of-file-bytes scheme) alongside these
// SHA-256 file-integrity values.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace guacamole_pdc {

struct KnownProfile {
  const char* name;
  // Real, confirmed runtime path on-device (see
  // guacamole_efsd_stock_modem_design.md item 9: confirmed via
  // vendor/etc/fstab.qcom - /vendor/firmware_mnt is the `modem`
  // partition mounted read-only).
  const char* runtime_path;
  uint32_t expected_size;
  std::array<uint8_t, 32> expected_sha256;
};

// The ONLY selector Cycle 2's PDC engine ever accepts for "which
// carrier's config to work with" - never a raw path, never a raw
// config ID/SHA string. See efs_write_targets.h's EfsWriteTarget for
// the exact same enforcement shape applied to EFS2 writes.
enum class Carrier { kSkt, kKt, kLgu };

const char* CarrierToString(Carrier carrier);

// Stock H.41 Korea SKT mcfg_sw.mbn. Confirmed present in the firmware
// extraction at
// modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/skt/commerci/mcfg_sw.mbn
// (size 41172, sha256 9ee13411bd223576b0bdd14823929ace3544f1f1c2eab57a23a30038bc33805e -
// both values computed directly from that file, not estimated). Real
// device PDC Load: PASS (Phase C0).
const KnownProfile& GetSktProfile();

// Stock H.41 Korea KT mcfg_sw.mbn (added 2026-09-24, Cycle 2 Section H).
// modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/kt/commerci/mcfg_sw.mbn
// (size 39544, sha256 0305b51ca7446db9bb16e78964f6c6520739281c228ff596987d23484b9cd101).
// Real device PDC Load: NOT YET ATTEMPTED (Gate W1 - see
// PHASE_D2_EFS2_WRITE_RESEARCH.md).
const KnownProfile& GetKtProfile();

// Stock H.41 Korea LGU mcfg_sw.mbn (added 2026-09-24, Cycle 2 Section H).
// modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/lgu/commerci/mcfg_sw.mbn
// (size 41088, sha256 c84bcf76cb626debee291d6ee9b87e83b7c686070efe541c28d6ef46b84819b2).
// Real device PDC Load: NOT YET ATTEMPTED (Gate W1).
const KnownProfile& GetLguProfile();

// The ONLY dispatch point from a `Carrier` enum value to its
// `KnownProfile` - mirrors efs_write_targets.h's TargetPath() pattern
// exactly.
const KnownProfile* GetProfileForCarrier(Carrier carrier);

}  // namespace guacamole_pdc
