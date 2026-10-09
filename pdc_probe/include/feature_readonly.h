#pragma once
#include "auto_selection_experiment.h"
#include "pdc_client.h"

namespace guacamole_pdc {
inline constexpr unsigned int kMsgGetFeature = 0x2d;
struct FeatureResult {
  bool valid = false;
  uint16_t error = 0;
  std::optional<uint32_t> token, selection_mode, selection_carrier, refresh_mode;
  std::optional<bool> flex_mapping;
  size_t extension_count = 0;
};
struct FeatureReadOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  uint16_t sync_result = 0, sync_error = 0;
  FeatureResult data;
};
std::vector<uint8_t> BuildGetFeatureRequest(FeatureSlot slot, uint32_t token);
FeatureResult ParseGetFeatureIndication(const std::vector<uint8_t>& bytes, uint32_t token);
std::optional<FeatureSlot> ParseFeatureReadCommand(const std::vector<std::string>& args);

// Single-request guard: permits ONLY exact Get Feature for the fixed typed slot.
// No Arm, arbitrary message, selection, load, feature-write or retry API.
class FeatureReadQcciGuard final : public IQcciShim {
 public:
  FeatureReadQcciGuard(IQcciShim& inner, FeatureSlot slot) : inner_(inner), slot_(slot) {}
  bool Load(std::string* e) override { return inner_.Load(e); }
  QmiServiceObject PdcGetServiceObject(int32_t a, int32_t b, int32_t c) override {
    return inner_.PdcGetServiceObject(a,b,c);
  }
  int ClientInitInstance(QmiServiceObject o, QmiIndicationCallback cb, void* data,
      uint32_t timeout, QmiClientHandle* h) override {
    return inner_.ClientInitInstance(o,cb,data,timeout,h);
  }
  int ClientRelease(QmiClientHandle h) override { return inner_.ClientRelease(h); }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override;
 private:
  IQcciShim& inner_;
  FeatureSlot slot_;
  bool consumed_ = false;
};

// Dedicated one-query client; no changes to established PdcClient mutation routing.
// Shim must outlive client. ClientRelease quiesces callback before destruction.
class FeatureReadClient {
 public:
  explicit FeatureReadClient(IQcciShim& shim) : shim_(shim) {}
  ~FeatureReadClient();
  FeatureReadClient(const FeatureReadClient&) = delete;
  FeatureReadClient& operator=(const FeatureReadClient&) = delete;
  FeatureReadOutcome Read(FeatureSlot slot, uint32_t timeout_ms);
 private:
  static void Callback(QmiClientHandle, unsigned int, void*, unsigned int, void*);
  IQcciShim& shim_;
  QmiClientHandle handle_ = nullptr;
  std::mutex call_mutex_, mutex_;
  std::condition_variable cv_;
  bool used_ = false, waiting_ = false;
  std::optional<std::vector<uint8_t>> indication_;
};
}
