// guacamole_efs_probe - whole-file chunked EFS2 reader. Orchestration
// layer on top of efs_wire.h/efs_client.h: STAT -> OPEN -> repeated READ
// (each capped at kMaxReadRequestBytes) -> CLOSE, with the STAT-reported
// size as the sole authoritative expected length (fail-closed: the
// actually-read byte count MUST equal it, or the whole read is FAILED,
// never returned as a silently-truncated/padded buffer). No retries
// anywhere - matching this project's standing policy (see
// PHASE_C0_DESIGN.md's chunk-protocol section): a single failed/timed-out
// chunk aborts the read as kFailed.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "efs_client.h"

namespace guacamole_efs {

enum class EfsReadFileState {
  kDone,
  kFailed,
};

struct EfsReadFileResult {
  EfsReadFileState state = EfsReadFileState::kFailed;
  std::string message;
  int32_t stat_size = 0;
  uint32_t total_bytes_read = 0;
  std::vector<uint8_t> data;
};

// Defensive hard cap on the STAT-reported size this reader will ever
// attempt to read into memory - this project's own trust boundary, NOT
// a confirmed EFS2/protocol maximum (none is known). 16 MiB is well
// above every real file size this project has actual evidence for (the
// largest known real file on this device is the real, captured
// `mcfg_sel_db.xml` at 57,655 bytes - see the Cycle 1 completion
// report's live baseline) while still bounding worst-case memory use
// if a STAT response is ever corrupted or a target turns out to be
// unexpectedly large. A file whose STAT size exceeds this is rejected
// outright, never silently truncated.
constexpr int32_t kMaxFileSizeBytes = 16 * 1024 * 1024;

EfsReadFileResult ReadWholeFile(IEfsClient* client, const std::string& path);

}  // namespace guacamole_efs
