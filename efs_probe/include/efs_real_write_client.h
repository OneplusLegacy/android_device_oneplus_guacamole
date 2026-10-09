// guacamole_efs_probe - real IEfsWriteClient implementation, built on
// the same H41_DEVICE_CONFIRMED DiagTransport::SendSubsysCommand() the
// read path uses. Cycle 2, Section E. Pure wiring: Build* ->
// SendSubsysCommand -> Parse*, exactly like EfsRealClient - see
// efs_real_client.h, whose EfsWireCallLog struct this file reuses
// verbatim (it was already generic, not read-specific).
//
// H41_DEVICE_NOT_YET_CONFIRMED: nothing in this codebase calls this
// class against a real device yet - see efs_write_wire.h's header
// comment and the Gate W1 report that introduced this file.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "diag_transport.h"
#include "efs_real_client.h"  // reuses EfsWireCallLog.
#include "efs_write_client.h"

namespace guacamole_efs {

class EfsRealWriteClient : public IEfsWriteClient {
 public:
  EfsRealWriteClient(DiagTransport* transport,
                      std::chrono::milliseconds timeout);

  MutationOpenResult OpenWrite(EfsWriteTarget target) override;
  MutationWriteResult Write(int32_t fd, uint32_t offset,
                             const std::vector<uint8_t>& data) override;
  MutationCloseResult Close(int32_t fd) override;

  // Same shape/purpose as EfsRealClient::CallLog() - every wire round
  // trip this instance has performed so far, oldest first.
  const std::vector<EfsWireCallLog>& CallLog() const { return call_log_; }

 private:
  DiagTransport* transport_;
  std::chrono::milliseconds timeout_;
  std::vector<EfsWireCallLog> call_log_;
};

}  // namespace guacamole_efs
