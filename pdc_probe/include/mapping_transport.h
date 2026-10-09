#pragma once
#include "mapping_experiment.h"

namespace guacamole_pdc {
// Defense in depth below PdcClient: no Load/Activate/Delete/List/raw-ID route.
// The optional selection target is fixed from validated CLI arguments.
class MappingQcciGuard final : public IQcciShim {
 public:
  MappingQcciGuard(IQcciShim& inner, std::optional<MappingTarget> target)
      : inner_(inner), target_(target) {}
  bool Load(std::string* e) override { return inner_.Load(e); }
  QmiServiceObject PdcGetServiceObject(int32_t a, int32_t b, int32_t c) override {
    return inner_.PdcGetServiceObject(a, b, c);
  }
  int ClientInitInstance(QmiServiceObject obj, QmiIndicationCallback cb, void* ctx,
      uint32_t ms, QmiClientHandle* h) override { return inner_.ClientInitInstance(obj, cb, ctx, ms, h); }
  int ClientRelease(QmiClientHandle h) override { return inner_.ClientRelease(h); }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override;
 private:
  IQcciShim& inner_;
  std::optional<MappingTarget> target_;
  bool attempted_ = false;
};
}
