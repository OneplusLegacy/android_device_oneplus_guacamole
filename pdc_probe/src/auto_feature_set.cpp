#include "auto_feature_set.h"
#include <chrono>
namespace guacamole_pdc {
bool AutoFeatureSetQcciGuard::Arm(const AutoSlot0Preflight& pre,
    const std::string& confirmation,const IAutoExperimentState& state) {
#if defined(__BIONIC__)
  if (!kDeviceAutoSlot0SendOpen) return false;
#endif
  if (arm_consumed_.exchange(true) || confirmation!=kAutoSlot0Confirmation ||
      !AutoSlot0PreflightVerified(pre) || !state.PendingDurable()) return false;
  armed_=true;
  return true;
}
int AutoFeatureSetQcciGuard::SendRawMsgSync(QmiClientHandle h,unsigned int msg,
    const void* request,unsigned int size,void* response,unsigned int capacity,
    unsigned int* received,unsigned int timeout) {
  const auto exact=BuildAutoFeatureRequest(FeatureSlot::kSlot0,true);
  if (msg!=kObservedAutoMessage || !request || size!=exact.size() ||
      std::vector<uint8_t>(static_cast<const uint8_t*>(request),
                           static_cast<const uint8_t*>(request)+size)!=exact) return -1;
  bool was_armed=true;
  if (!armed_.compare_exchange_strong(was_armed,false)) return -1;
  attempted_=true; // consume permanently BEFORE crossing the transport boundary
  return inner_.SendRawMsgSync(h,msg,request,size,response,capacity,received,timeout);
}
AutoFeatureSetClient::~AutoFeatureSetClient() {
  if (handle_) guard_.ClientRelease(handle_); // callbacks quiesced before member destruction
}
void AutoFeatureSetClient::Callback(QmiClientHandle,unsigned int msg,void* raw,
    unsigned int size,void* context) {
  if (msg!=kObservedAutoMessage) return;
  auto* self=static_cast<AutoFeatureSetClient*>(context);
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (!self->waiting_) return;
    if (!raw || size==0 || size>12 || self->indication_) self->invalid_=true;
    else {
      const auto* p=static_cast<const uint8_t*>(raw);
      self->indication_=std::vector<uint8_t>(p,p+size);
    }
  }
  self->cv_.notify_one();
}
AutoFeatureSetResult AutoFeatureSetClient::EnableSlot0(uint32_t timeout) {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  AutoFeatureSetResult r;
  if (used_ || !guard_.Armed()) { r.status=ProbeStatus::kMalformedResponse; return r; }
  used_=true;
  std::string error;
  if (!guard_.Load(&error)) return r;
  auto object=guard_.PdcGetServiceObject(1,15,6);
  if (!object || guard_.ClientInitInstance(object,&Callback,this,timeout,&handle_)!=kQmiNoErr || !handle_) return r;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_=true; // indication can precede synchronous ACK
  }
  const auto bytes=BuildAutoFeatureRequest(FeatureSlot::kSlot0,true);
  uint8_t ack[64]{}; unsigned int received=0;
  const int rc=guard_.SendRawMsgSync(handle_,kObservedAutoMessage,bytes.data(),bytes.size(),
                                   ack,sizeof(ack),&received,timeout);
  r.mutation_attempted=guard_.Attempted();
  if (received<=sizeof(ack)) r.sync=ParseObservedAutoSync(std::vector<uint8_t>(ack,ack+received));
  std::unique_lock<std::mutex> lock(mutex_);
  if (rc!=kQmiNoErr) r.status=ProbeStatus::kRequestSendFailed;
  else if (!r.sync.valid) r.status=ProbeStatus::kMalformedResponse;
  else if (r.sync.result || r.sync.error) r.status=ProbeStatus::kRequestRejected;
  else if (!cv_.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return invalid_ || indication_.has_value();}))
    r.status=ProbeStatus::kTimeout;
  else if (invalid_) r.status=ProbeStatus::kMalformedResponse;
  else {
    r.indication=ParseObservedAutoIndication(*indication_);
    r.status=!r.indication.valid ? ProbeStatus::kMalformedResponse :
        r.indication.error ? ProbeStatus::kIndicationRejected : ProbeStatus::kOk;
  }
  waiting_=false;
  // Preserve a well-formed early indication even when ACK/send failed, without
  // letting it override the sync failure classification.
  if (!invalid_ && indication_ && !r.indication.valid)
    r.indication=ParseObservedAutoIndication(*indication_);
  r.ambiguous=r.mutation_attempted && (r.status==ProbeStatus::kRequestSendFailed ||
      r.status==ProbeStatus::kMalformedResponse || r.status==ProbeStatus::kTimeout);
  return r;
}
}
