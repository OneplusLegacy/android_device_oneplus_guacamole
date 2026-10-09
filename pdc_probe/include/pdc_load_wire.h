// guacamole_pdc_probe - PDC Load Config wire format. THIS IS THE WRITE
// PATH. Deliberately kept in its own file, separate from pdc_wire.h
// (List Configs / Get Config Info / Get Selected Config, all pure
// reads), so the boundary between read-only Phase B/B2 code and the
// Phase C0 write path is structurally obvious, not just a comment.
//
// Message ID and TLV layout: data/qmi-service-pdc.json (same public,
// cross-device-validated libqmi source used for the rest of this
// project's PDC wire format - see pdc_wire.h for the full provenance
// story).
//
// CONFIG ID SCHEME - the single most safety-critical fact in this file:
// CONFIRMED (not guessed) as SHA-1 of the ENTIRE config file's raw
// bytes. Two independent lines of evidence converge exactly:
//   1. libqmi's own public, production reference client (qmicli-pdc.c)
//      computes exactly this: g_checksum_new(G_CHECKSUM_SHA1) over the
//      whole mapped file, and sends that digest as the Config Chunk's
//      Id field on every chunk.
//   2. Empirically verified against THIS real device: the real
//      Common-Commercial config's actual reported id
//      (0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493, from a real `info`
//      run) is the exact byte-for-byte SHA-1 of a real 24508-byte .mbn
//      file found in this firmware
//      (generic/oppo_exp/common/commerci/mcfg_sw.mbn).
// See research_pdc_load_config_id_scheme.md for the full evidence. The
// modem does NOT generate/return a new id anywhere in the Load Config
// indication (only Received/Remaining Size/Frame Reset) - the client
// must compute and supply it, identically on every chunk.
//
// CHUNK SIZE: libqmi's reference implementation uses 1024 bytes
// (LOAD_CONFIG_CHUNK_SIZE). H.41 disassembly could not confirm this
// exact value for qcrild's own chunking loop (logging boilerplate
// obscured it, tracked honestly as UNRESOLVED in the research file) -
// kLoadChunkSize here uses the same conservative 1024-byte value as the
// public reference, which is the safest available choice given the
// wire format's guint16 chunk-length field could in principle go much
// higher but transport MTU realistically can't be assumed to support that.
//
// TOTAL SIZE: resent identically (the whole file's size) on EVERY
// chunk, not just the first - confirmed from the reference client's
// `load_config_input_create_chunk()`, which re-reads the mapped file's
// length on each call.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "pdc_wire.h"  // ConfigType, IndicationOutcome, kTlv* shared constants

namespace guacamole_pdc {

constexpr uint32_t kMsgLoadConfig = 0x0026;

constexpr uint8_t kTlvLoadConfigReqChunk = 0x01;
constexpr uint8_t kTlvLoadConfigIndReceived = 0x11;
constexpr uint8_t kTlvLoadConfigIndRemainingSize = 0x12;
constexpr uint8_t kTlvLoadConfigIndFrameReset = 0x13;

// See file header - CONFIRMED against a real reference implementation
// and empirically against this real device, not a guess.
constexpr size_t kLoadChunkSize = 1024;

struct LoadConfigChunkResult {
  bool ok = false;
  uint16_t error_status = 0;
  uint16_t error_code = 0;
  uint32_t received = 0;
  bool has_received = false;
  uint32_t remaining_size = 0;
  bool has_remaining_size = false;
  bool frame_reset = false;
  bool has_frame_reset = false;
};

// Builds ONE Load Config chunk request. `id` is the full SHA-1 digest of
// the config file (see file header) - sent identically on every chunk.
// `total_size` is the whole file's size, also sent identically on every
// chunk. `chunk_bytes` is just this chunk's slice of the file (at most
// kLoadChunkSize bytes, though this function does not itself enforce
// that cap - the caller's chunking loop is responsible for slicing
// correctly; this function only rejects a chunk that would overflow the
// wire format's own guint16 chunk-length limit).
std::vector<uint8_t> BuildLoadConfigChunkRequest(
    uint32_t token, ConfigType type, const std::vector<uint8_t>& id,
    uint32_t total_size, const uint8_t* chunk_bytes, size_t chunk_len);

// Parses a Load Config indication body, same token-matching contract as
// every other Parse* function in this project.
LoadConfigChunkResult ParseLoadConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token);

IndicationOutcome ClassifyLoadConfigChunkResult(
    const LoadConfigChunkResult& result);

}  // namespace guacamole_pdc
