// guacamole_efs_probe - abstract EFS2 round-trip interface. Mirrors
// guacamole_pdc::IPdcClient's role: lets higher-level logic (EfsFileReader,
// future CLI commands) be host-tested against a fake, without linking
// libdiag.so or a real DIAG transport. See diag_transport.h for why the
// REAL implementation of this interface does not exist yet in Phase D0
// (the send/receive round trip is not implemented pending resolution of
// the callback-correlation mechanics).

#pragma once

#include <cstdint>
#include <string>

#include "efs_wire.h"

namespace guacamole_efs {

class IEfsClient {
 public:
  virtual ~IEfsClient() = default;

  virtual EfsStatResult Stat(const std::string& path) = 0;
  virtual EfsOpenResult OpenReadOnly(const std::string& path) = 0;
  virtual EfsReadResult Read(int32_t fd, uint32_t nbyte, uint32_t offset) = 0;
  virtual EfsCloseResult Close(int32_t fd) = 0;
  virtual EfsOpenDirResult OpenDir(const std::string& path) = 0;
  virtual EfsReadDirResult ReadDir(uint32_t dirp, int32_t seqno) = 0;
  virtual EfsCloseDirResult CloseDir(uint32_t dirp) = 0;
};

}  // namespace guacamole_efs
