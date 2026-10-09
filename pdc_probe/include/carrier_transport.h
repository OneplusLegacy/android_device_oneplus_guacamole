#pragma once
#include "carrier_experiment.h"
namespace guacamole_pdc {
// Pure exact wire comparison; caller must independently validate full file identity.
bool CarrierLoadRequestMatches(CarrierTarget target, const std::vector<uint8_t>& file,
    size_t offset, uint32_t token, const std::vector<uint8_t>& request);
// Explicit permit issued only after state-machine preflight. No Activate/Delete
// message can pass. Load's single logical transfer has one permit per exact chunk.
class CarrierQcciGuard final : public IQcciShim {
 public:
  CarrierQcciGuard(IQcciShim& inner, const CarrierRequest& request)
      : inner_(inner), request_(request) {}
  bool Arm(const CarrierFile& file);
  bool ArmH41(const CarrierFile& file, const CarrierReport& evidence);
  void AcknowledgeChunk(const LoadConfigChunkOutcome& outcome);
  bool Load(std::string* e) override { return inner_.Load(e); }
  QmiServiceObject PdcGetServiceObject(int32_t a, int32_t b, int32_t c) override {
    return inner_.PdcGetServiceObject(a, b, c);
  }
  int ClientInitInstance(QmiServiceObject o, QmiIndicationCallback cb, void* ctx,
      uint32_t ms, QmiClientHandle* h) override { return inner_.ClientInitInstance(o, cb, ctx, ms, h); }
  int ClientRelease(QmiClientHandle h) override { return inner_.ClientRelease(h); }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override;
 private:
  IQcciShim& inner_;
  CarrierRequest request_;
  std::vector<uint8_t> bytes_;
  bool arm_consumed_ = false, enabled_ = false, waiting_ = false;
  size_t offset_ = 0, pending_size_ = 0;
};
class CarrierClientAdapter final : public ICarrierClient {
 public:
  CarrierClientAdapter(IPdcClient& client, CarrierQcciGuard& guard, const CarrierFile& file)
      : client_(client), guard_(guard), file_(file) {}
  GetSelectedConfigOutcome Selected() override;
  GetConfigInfoOutcome Info(CarrierTarget target) override;
  ListConfigsOutcome List() override;
  GetConfigInfoOutcome SyntheticInfo() override;
  LoadConfigChunkOutcome Chunk(CarrierTarget target, size_t offset) override;
  SetSelectedConfigOutcome Select(CarrierTarget target) override;
 private:
  IPdcClient& client_;
  CarrierQcciGuard& guard_;
  const CarrierFile& file_;
};
}
