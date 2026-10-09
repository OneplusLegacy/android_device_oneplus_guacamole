#pragma once
#include "auto_slot0_experiment.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
namespace guacamole_pdc {
class AutoFeatureSetQcciGuard final : public IQcciShim {
 public:
  explicit AutoFeatureSetQcciGuard(IQcciShim& inner) : inner_(inner) {}
  bool Arm(const AutoSlot0Preflight&, const std::string&, const IAutoExperimentState&);
  bool Armed() const { return armed_.load(); }
  bool Attempted() const { return attempted_.load(); }
  bool Load(std::string* e) override { return inner_.Load(e); }
  QmiServiceObject PdcGetServiceObject(int32_t a,int32_t b,int32_t c) override { return inner_.PdcGetServiceObject(a,b,c); }
  int ClientInitInstance(QmiServiceObject o,QmiIndicationCallback cb,void* ctx,uint32_t ms,QmiClientHandle* h) override {
    return inner_.ClientInitInstance(o,cb,ctx,ms,h);
  }
  int ClientRelease(QmiClientHandle h) override { return inner_.ClientRelease(h); }
  int SendRawMsgSync(QmiClientHandle,unsigned int,const void*,unsigned int,void*,unsigned int,unsigned int*,unsigned int) override;
 private:
  IQcciShim& inner_;
  std::atomic<bool> arm_consumed_{false}, armed_{false}, attempted_{false};
};
// One client/one outstanding request; owns no token. A fresh QCCI control point
// plus the persistent process lock isolates this untagged unicast indication.
class AutoFeatureSetClient {
 public:
  explicit AutoFeatureSetClient(AutoFeatureSetQcciGuard& guard) : guard_(guard) {}
  ~AutoFeatureSetClient();
  AutoFeatureSetClient(const AutoFeatureSetClient&) = delete;
  AutoFeatureSetClient& operator=(const AutoFeatureSetClient&) = delete;
  AutoFeatureSetResult EnableSlot0(uint32_t timeout_ms);
 private:
  static void Callback(QmiClientHandle,unsigned int,void*,unsigned int,void*);
  AutoFeatureSetQcciGuard& guard_;
  QmiClientHandle handle_ = nullptr;
  std::mutex call_mutex_, mutex_;
  std::condition_variable cv_;
  bool used_=false, waiting_=false, invalid_=false;
  std::optional<std::vector<uint8_t>> indication_;
};
}
