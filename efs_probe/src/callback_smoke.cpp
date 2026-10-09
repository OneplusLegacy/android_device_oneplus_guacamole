#include "callback_smoke.h"

#include <vector>

#include "diag_loopback_packet.h"

namespace guacamole_efs {

TransportResult RunCallbackSmoke(DiagTransport* transport,
                                  std::chrono::milliseconds timeout) {
  std::vector<uint8_t> packet(kH41LoopbackPacket.begin(),
                               kH41LoopbackPacket.end());
  return transport->SendSubsysCommand(/*proc=*/0, packet, timeout);
}

}  // namespace guacamole_efs
