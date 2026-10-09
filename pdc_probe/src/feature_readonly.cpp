#include "feature_readonly.h"
#include <array>
#include <chrono>

namespace guacamole_pdc {
namespace {
constexpr uint32_t kToken = 1;  // One request per independently initialized client.
bool ValidSlot(FeatureSlot slot) {
  return slot == FeatureSlot::kSlot0 || slot == FeatureSlot::kSlot1;
}
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
}
std::vector<uint8_t> BuildGetFeatureRequest(FeatureSlot slot, uint32_t token) {
  if (!ValidSlot(slot) || token == 0) return {};
  // Optional token is explicitly defined for Get Feature in PDC1.9 and H.41 IDL.
  return {1,4,0,static_cast<uint8_t>(slot),0,0,0,0x10,4,0,
      uint8_t(token),uint8_t(token>>8),uint8_t(token>>16),uint8_t(token>>24)};
}
FeatureResult ParseGetFeatureIndication(const std::vector<uint8_t>& b, uint32_t token) {
  FeatureResult r;
  if (b.size() > 80 || token == 0) return r; // H.41 IDL maximum
  std::array<bool,256> seen{};
  for (size_t i=0; i<b.size();) {
    if (b.size()-i < 3) return {};
    const auto type=b[i]; const size_t n=size_t(b[i+1]) | size_t(b[i+2])<<8; i+=3;
    if (n>b.size()-i || seen[type]) return {};
    seen[type]=true;
    const auto* p=b.data()+i;
    if (type==1 && n==2) r.error=uint16_t(p[0]) | uint16_t(p[1])<<8;
    else if (type==0x10 && n==4) r.token=U32(p);
    else if (type==0x11 && n==4) r.selection_mode=U32(p);
    else if (type==0x12 && n==4) r.selection_carrier=U32(p);
    else if (type==0x13 && n==1 && p[0]<=1) r.flex_mapping=p[0]!=0;
    else if (type==0x14 && n==4) r.refresh_mode=U32(p);
    // Additional optional fields exist in local H.41 IDL. Validate width, do not
    // invent their semantics. They do not change the AUTO feature classification.
    else if ((type>=0x15 && type<=0x18 && n==4) ||
             (type>=0x19 && type<=0x1b && n==2)) ++r.extension_count;
    else return {};
    i+=n;
  }
  r.valid=seen[1] && r.token && *r.token==token;
  return r;
}
std::optional<FeatureSlot> ParseFeatureReadCommand(const std::vector<std::string>& a) {
  if (a==std::vector<std::string>{"get-auto-feature-readonly","0"}) return FeatureSlot::kSlot0;
  if (a==std::vector<std::string>{"get-auto-feature-readonly","1"}) return FeatureSlot::kSlot1;
  return std::nullopt;
}
int FeatureReadQcciGuard::SendRawMsgSync(QmiClientHandle h, unsigned int msg,
    const void* request, unsigned int size, void* response, unsigned int capacity,
    unsigned int* received, unsigned int timeout) {
  if (consumed_ || msg!=kMsgGetFeature || !request || size!=14 || !ValidSlot(slot_)) return -1;
  const auto* p=static_cast<const uint8_t*>(request);
  const uint32_t token=U32(p+10);
  const auto expected=BuildGetFeatureRequest(slot_,token);
  if (expected.empty() || std::vector<uint8_t>(p,p+size)!=expected) return -1;
  consumed_=true; // consume even on send failure; no retry
  return inner_.SendRawMsgSync(h,msg,request,size,response,capacity,received,timeout);
}
FeatureReadClient::~FeatureReadClient() {
  if (handle_) shim_.ClientRelease(handle_);
}
void FeatureReadClient::Callback(QmiClientHandle, unsigned int msg, void* raw,
    unsigned int size, void* context) {
  if (msg!=kMsgGetFeature || !raw || size>80) return;
  const auto* data=static_cast<const uint8_t*>(raw);
  const auto token=PeekIndicationToken(data,size);
  if (!token || *token!=kToken) return; // unrelated/unrouteable cannot complete wait
  auto* self=static_cast<FeatureReadClient*>(context);
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (!self->waiting_ || self->indication_) return;
    self->indication_=std::vector<uint8_t>(data,data+size);
  }
  self->cv_.notify_one();
}
FeatureReadOutcome FeatureReadClient::Read(FeatureSlot slot, uint32_t timeout_ms) {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  FeatureReadOutcome r;
  if (used_ || !ValidSlot(slot)) { r.status=ProbeStatus::kMalformedResponse; return r; }
  used_=true;
  std::string error;
  if (!shim_.Load(&error)) return r;
  auto object=shim_.PdcGetServiceObject(1,15,6);
  if (!object || shim_.ClientInitInstance(object,&Callback,this,timeout_ms,&handle_)!=kQmiNoErr || !handle_) return r;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_=true;
  }
  const auto request=BuildGetFeatureRequest(slot,kToken);
  uint8_t ack[64]{}; unsigned int received=0;
  const int rc=shim_.SendRawMsgSync(handle_,kMsgGetFeature,request.data(),request.size(),
      ack,sizeof(ack),&received,timeout_ms);
  const auto sync = received <= sizeof(ack) ?
      ParseObservedAutoSync(std::vector<uint8_t>(ack, ack + received)) : AutoSyncResult{};
  r.sync_result = sync.result;
  r.sync_error = sync.error;
  std::unique_lock<std::mutex> lock(mutex_);
  if (rc!=kQmiNoErr) r.status=ProbeStatus::kRequestSendFailed;
  else if (!sync.valid)
    r.status=ProbeStatus::kMalformedResponse;
  else if (r.sync_result || r.sync_error) r.status=ProbeStatus::kRequestRejected;
  else if (!cv_.wait_for(lock,std::chrono::milliseconds(timeout_ms),[&]{return indication_.has_value();}))
    r.status=ProbeStatus::kTimeout;
  else {
    r.data=ParseGetFeatureIndication(*indication_,kToken);
    r.status=!r.data.valid ? ProbeStatus::kMalformedResponse :
        r.data.error ? ProbeStatus::kIndicationRejected : ProbeStatus::kOk;
  }
  waiting_=false;
  return r;
}
}
