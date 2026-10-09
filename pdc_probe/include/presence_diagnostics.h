#pragma once
#include "carrier_experiment.h"
#include <array>
namespace guacamole_pdc {
// Diagnostic ID is fixed, not presumed absent, never allowed in a mutation.
const std::vector<uint8_t>& PresenceDiagnosticId();
std::vector<uint8_t> PresenceDiagnosticQueryId(size_t index);
struct PresenceDiagnostics {
  GetSelectedConfigOutcome selected;
  ListConfigsOutcome list;
  bool list_observed = false;
  std::array<bool, 5> info_observed{};
  bool stopped_on_transport_failure = false;
  std::array<GetConfigInfoOutcome, 5> info;
  std::array<bool, 5> listed{}; // advisory raw SOFTWARE membership only
};
PresenceDiagnostics DiagnosePresence(IPdcClient& client);
bool IsPresenceDiagnosticsCommand(const std::vector<std::string>& args);
// Independent immutable read-only transport boundary; never armable.
class PresenceQcciGuard final : public IQcciShim {
 public:
  explicit PresenceQcciGuard(IQcciShim& inner) : inner_(inner) {}
  bool Load(std::string* e) override { return inner_.Load(e); } // library load, not PDC Load
  QmiServiceObject PdcGetServiceObject(int32_t a, int32_t b, int32_t c) override {
    return inner_.PdcGetServiceObject(a,b,c);
  }
  int ClientInitInstance(QmiServiceObject o, QmiIndicationCallback cb, void* ctx,
      uint32_t ms, QmiClientHandle* h) override { return inner_.ClientInitInstance(o,cb,ctx,ms,h); }
  int ClientRelease(QmiClientHandle h) override { return inner_.ClientRelease(h); }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override;
 private:
  IQcciShim& inner_;
};
}
