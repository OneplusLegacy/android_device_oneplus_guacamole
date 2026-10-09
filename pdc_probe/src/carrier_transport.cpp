#include "carrier_transport.h"
#include "presence_diagnostics.h"
#include <algorithm>
namespace guacamole_pdc {
bool CarrierLoadRequestMatches(CarrierTarget target, const std::vector<uint8_t>& file,
    size_t offset, uint32_t token, const std::vector<uint8_t>& request) {
  if ((target != CarrierTarget::kKt && target != CarrierTarget::kLgu) ||
      file.empty() || file.size() > 1024 * 1024 || offset >= file.size()) return false;
  const size_t length = std::min(kLoadChunkSize, file.size() - offset);
  return request == BuildLoadConfigChunkRequest(token, ConfigType::kSoftware,
      CarrierId(target), file.size(), file.data() + offset, length);
}

bool CarrierQcciGuard::Arm(const CarrierFile& file) {
  if (arm_consumed_) return false;
  arm_consumed_ = true;
  if (!ValidCarrierRequest(request_)) return false;
  if (request_.operation == CarrierOperation::kLoad) {
    if (!file.verified || !VerifyCarrierBytes(request_.target, file.bytes)) return false;
    bytes_ = file.bytes; enabled_ = true; return true;
  }
  if (request_.operation == CarrierOperation::kSelect) { enabled_ = true; return true; }
  return false;
}
bool CarrierQcciGuard::ArmH41(const CarrierFile& file, const CarrierReport& r) {
  if (arm_consumed_) return false;
  arm_consumed_ = true;
  if (request_.operation != CarrierOperation::kH41Load || !ValidCarrierRequest(request_) ||
      !file.verified || !VerifyCarrierBytes(request_.target,file.bytes)) return false;
  // Protocol-absent uses the established path; 41 requires the complete same-run evidence.
  const auto n = static_cast<size_t>(request_.target);
  const bool protocol_absent = r.pre_observed && r.queried[0] && r.queried[1] && r.queried[n] &&
      r.pre.status == ProbeStatus::kOk && r.pre.data.ok && !r.pre.data.error_status && !r.pre.data.error_code &&
      r.pre.data.active_id == CarrierId(CarrierTarget::kCommon) && !r.pre.data.pending_id &&
      CarrierInfoVerified(CarrierTarget::kCommon,r.info[0]) && CarrierInfoVerified(CarrierTarget::kSkt,r.info[1]) &&
      CarrierPresence(r.info[n]) == ConfigPresence::kAbsent;
  if (!protocol_absent && !H41LoadCandidate(request_.target,r,file)) return false;
  bytes_=file.bytes; enabled_=true; return true;
}
void CarrierQcciGuard::AcknowledgeChunk(const LoadConfigChunkOutcome& o) {
  if (!waiting_ || !CarrierChunkAccepted(o, pending_size_, bytes_.size() - offset_ - pending_size_)) {
    enabled_ = false; waiting_ = false; return;
  }
  offset_ += pending_size_;
  waiting_ = false;
  enabled_ = offset_ < bytes_.size();
}
int CarrierQcciGuard::SendRawMsgSync(QmiClientHandle h, unsigned int msg,
    const void* request, unsigned int size, void* response, unsigned int capacity,
    unsigned int* received, unsigned int timeout) {
  if (!request || !size || size > 2048) return -1;
  const auto* data = static_cast<const uint8_t*>(request);
  const auto token = PeekIndicationToken(data, size);
  if (!token) return -1;
  const std::vector<uint8_t> bytes(data, data + size);
  bool allowed = false;
  if (msg == kMsgGetSelectedConfig) allowed = bytes == BuildGetSelectedConfigRequest(*token, ConfigType::kSoftware);
  else if (msg == kMsgListConfigs && request_.operation == CarrierOperation::kH41Load)
    allowed = bytes == BuildListConfigsRequest(*token,ConfigType::kSoftware);
  else if (msg == kMsgGetConfigInfo) {
    if (request_.operation == CarrierOperation::kH41Load)
      allowed = bytes == BuildGetConfigInfoRequest(*token,ConfigType::kSoftware,PresenceDiagnosticId());
    for (auto t : {CarrierTarget::kCommon, CarrierTarget::kSkt, CarrierTarget::kKt, CarrierTarget::kLgu})
      allowed = allowed || bytes == BuildGetConfigInfoRequest(*token, ConfigType::kSoftware, CarrierId(t));
  } else if (enabled_ && !waiting_ && msg == kMsgSetSelectedConfig && request_.operation == CarrierOperation::kSelect) {
    allowed = bytes == BuildSetSelectedConfigRequest(*token, ConfigType::kSoftware, CarrierId(request_.target));
    if (allowed) enabled_ = false; // consume before send, including transport failure
  } else if (enabled_ && !waiting_ && msg == kMsgLoadConfig && (request_.operation == CarrierOperation::kLoad || request_.operation == CarrierOperation::kH41Load) && offset_ < bytes_.size()) {
    const size_t length = std::min(kLoadChunkSize, bytes_.size() - offset_);
    allowed = CarrierLoadRequestMatches(request_.target, bytes_, offset_, *token, bytes);
    if (allowed) { enabled_ = false; waiting_ = true; pending_size_ = length; }
  }
  if (!allowed) return -1;
  return inner_.SendRawMsgSync(h, msg, request, size, response, capacity, received, timeout);
}
GetSelectedConfigOutcome CarrierClientAdapter::Selected() {
  return client_.GetSelectedConfig(ConfigType::kSoftware, 5000);
}
GetConfigInfoOutcome CarrierClientAdapter::Info(CarrierTarget t) {
  if (CarrierId(t).empty()) return {};
  return client_.GetConfigInfo(ConfigType::kSoftware, CarrierId(t), 5000);
}
ListConfigsOutcome CarrierClientAdapter::List() { return client_.ListConfigs(5000); }
GetConfigInfoOutcome CarrierClientAdapter::SyntheticInfo() {
  return client_.GetConfigInfo(ConfigType::kSoftware,PresenceDiagnosticId(),5000);
}
LoadConfigChunkOutcome CarrierClientAdapter::Chunk(CarrierTarget t, size_t offset) {
  if (!file_.verified || offset >= file_.bytes.size() || CarrierId(t).empty()) return {};
  const size_t length = std::min(kLoadChunkSize, file_.bytes.size() - offset);
  const auto r = client_.LoadConfigChunk(ConfigType::kSoftware, CarrierId(t), file_.bytes.size(),
      file_.bytes.data() + offset, length, 5000);
  guard_.AcknowledgeChunk(r);
  return r;
}
SetSelectedConfigOutcome CarrierClientAdapter::Select(CarrierTarget t) {
  if (CarrierId(t).empty()) return {};
  return client_.SetSelectedConfig(ConfigType::kSoftware, CarrierId(t), 5000);
}
}
