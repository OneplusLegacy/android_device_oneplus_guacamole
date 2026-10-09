#include "mapping_transport.h"
namespace guacamole_pdc {
int MappingQcciGuard::SendRawMsgSync(QmiClientHandle h, unsigned int msg,
    const void* request, unsigned int size, void* response, unsigned int capacity,
    unsigned int* received, unsigned int timeout) {
  if (!request || size == 0 || size > 256) return -1;
  const auto* p = static_cast<const uint8_t*>(request);
  const auto token = PeekIndicationToken(p, size);
  if (!token) return -1;
  const std::vector<uint8_t> bytes(p, p + size);
  bool allowed = false;
  if (msg == kMsgGetSelectedConfig) {
    allowed = bytes == BuildGetSelectedConfigRequest(*token, ConfigType::kSoftware);
  } else if (msg == kMsgGetConfigInfo) {
    for (auto t : {MappingTarget::kCommon, MappingTarget::kSkt})
      allowed = allowed || bytes == BuildGetConfigInfoRequest(*token, ConfigType::kSoftware, MappingId(t));
  } else if (msg == kMsgSetSelectedConfig && target_ && !attempted_ && !MappingId(*target_).empty()) {
    allowed = bytes == BuildSetSelectedConfigRequest(*token, ConfigType::kSoftware, MappingId(*target_));
    if (allowed) attempted_ = true;  // consume before sending, even if send fails
  }
  if (!allowed) return -1;
  return inner_.SendRawMsgSync(h, msg, request, size, response, capacity, received, timeout);
}
}
