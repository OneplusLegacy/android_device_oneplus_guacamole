// guacamole_efs_probe - real IEfsClient implementation, built entirely
// on the now H41_DEVICE_CONFIRMED `DiagTransport::SendSubsysCommand()`
// (see PHASE_D0_EFS2_RESEARCH.md's `LOOPBACK_ROUNDTRIP_DEVICE_RESULT`)
// and `efs_wire.h`'s codec. No new DIAG knowledge here - this class is
// pure wiring: Build* -> SendSubsysCommand -> Parse*, one method per
// EFS2 read-only operation, matching `IEfsClient`'s contract exactly.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "diag_transport.h"
#include "efs_client.h"

namespace guacamole_efs {

// One raw wire round trip, recorded in the exact order EfsRealClient
// performed it - this is the CLI's ONLY source for the
// request_hex=/response_hex=/transport_status=/diag_errno= fields
// item H of the Cycle 1 review requires, since IEfsClient's own
// structured results (EfsStatResult etc.) deliberately do not carry
// raw bytes. Never touched by anything other than EfsRealClient - the
// fakes used by host tests have no need for it and don't produce it.
struct EfsWireCallLog {
  std::string label;  // "STAT", "OPEN", "READ", "CLOSE", "OPENDIR",
                       // "READDIR", or "CLOSEDIR" - matches the EFS2
                       // command this call issued, not the snapshot
                       // step name (a single step can issue several of
                       // these, e.g. a directory listing is OPENDIR +
                       // N*READDIR + CLOSEDIR).
  std::vector<uint8_t> request;
  TransportStatus transport_status = TransportStatus::kInitFailed;
  // Exactly what the transport's callback delivered, before any
  // de-framing (see diag_frame.h) - populated whenever a correlated
  // callback arrived at all (transport_status kOk or
  // kFrameIntegrityFailed), empty otherwise.
  std::vector<uint8_t> raw_response;
  // The NORMALIZED ("logical") response - what `parsed_ok`/`diag_errno`
  // below and every Parse* function actually reflect. Only meaningful
  // when transport_status == kOk (a kFrameIntegrityFailed call never
  // reaches a parser at all - see EfsRealClient's method bodies).
  // Named `logical_response` (not `response`) specifically so it is
  // never confused with `raw_response` when reading a log - the
  // 2026-09-23 preflight finding that motivated this whole struct's
  // raw/logical split was exactly this kind of ambiguity (see
  // diag_frame.h's header comment).
  std::vector<uint8_t> logical_response;
  // Framing/CRC metadata - mirrors diag_frame.h's DiagFrameResult via
  // DiagTransport::TransportResult. Only meaningful alongside a
  // populated raw_response.
  bool had_hdlc_trailer = false;
  bool crc_valid = false;
  uint16_t crc_expected = 0;
  uint16_t crc_received = 0;
  // HDLC byte-stuffing metadata (2026-09-23 real-device finding, a
  // real READDIR response's CRC byte needed unescaping - see
  // diag_frame.h) - only meaningful alongside a populated raw_response.
  bool escape_present = false;
  int escape_count = 0;
  size_t unescaped_len = 0;
  bool parsed_ok = false;  // the matching Parse* function's own `.ok`
                            // - structurally valid, NOT "EFS success".
                            // Always false when transport_status is
                            // anything other than kOk - a parser is
                            // never invoked otherwise.
  int32_t diag_errno = 0;  // only meaningful when parsed_ok is true.
};

// `transport` must already be Init()-ialized by the caller and must
// outlive this object - this class does not own its lifecycle, mirrors
// every other orchestration class in this project (RunCallbackSmoke,
// RunStatMcfg). `proc` is fixed at 0 (local MSM) for every call -
// matching every EFS2/loopback request this project has traced or
// sent; there is no remote/MDM routing support anywhere in this
// module.
class EfsRealClient : public IEfsClient {
 public:
  EfsRealClient(DiagTransport* transport, std::chrono::milliseconds timeout);

  EfsStatResult Stat(const std::string& path) override;
  EfsOpenResult OpenReadOnly(const std::string& path) override;
  EfsReadResult Read(int32_t fd, uint32_t nbyte, uint32_t offset) override;
  EfsCloseResult Close(int32_t fd) override;
  EfsOpenDirResult OpenDir(const std::string& path) override;
  EfsReadDirResult ReadDir(uint32_t dirp, int32_t seqno) override;
  EfsCloseDirResult CloseDir(uint32_t dirp) override;

  // Every wire round trip this instance has performed so far, oldest
  // first. Grows by exactly one entry per IEfsClient call, whether that
  // call succeeded, failed, or never reached the transport (a Build*
  // refusal still logs an empty `request`/kInitFailed entry, so the
  // log's entry count always matches the call count).
  const std::vector<EfsWireCallLog>& CallLog() const { return call_log_; }

 private:
  DiagTransport* transport_;
  std::chrono::milliseconds timeout_;
  std::vector<EfsWireCallLog> call_log_;
};

}  // namespace guacamole_efs
