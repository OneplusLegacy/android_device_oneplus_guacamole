#include "presence_diagnostics.h"
namespace guacamole_pdc {
const std::vector<uint8_t>& PresenceDiagnosticId() {
  static const std::vector<uint8_t> id{
      0x47,0x55,0x41,0x43,0x41,0x4d,0x4f,0x4c,0x45,0x2d,
      0x50,0x44,0x43,0x2d,0x44,0x49,0x41,0x47,0x30,0x31};
  return id;
}
std::vector<uint8_t> PresenceDiagnosticQueryId(size_t i) {
  constexpr CarrierTarget targets[]{CarrierTarget::kCommon, CarrierTarget::kSkt,
      CarrierTarget::kKt, CarrierTarget::kLgu};
  if (i < 4) return CarrierId(targets[i]);
  return i == 4 ? PresenceDiagnosticId() : std::vector<uint8_t>{};
}
bool IsPresenceDiagnosticsCommand(const std::vector<std::string>& args) {
  return args == std::vector<std::string>{"diagnose-presence"};
}
PresenceDiagnostics DiagnosePresence(IPdcClient& client) {
  PresenceDiagnostics r;
  r.selected = client.GetSelectedConfig(ConfigType::kSoftware, 5000);
  const auto usable = [](ProbeStatus s) {
    return s == ProbeStatus::kOk || s == ProbeStatus::kRequestRejected ||
        s == ProbeStatus::kIndicationRejected;
  };
  if (!usable(r.selected.status)) { r.stopped_on_transport_failure = true; return r; }
  r.list_observed = true;
  r.list = client.ListConfigs(5000);
  if (!usable(r.list.status)) { r.stopped_on_transport_failure = true; return r; }
  for (size_t i = 0; i < r.info.size(); ++i) {
    const auto id = PresenceDiagnosticQueryId(i);
    for (const auto& entry : r.list.data.configs)
      if (entry.type == ConfigType::kSoftware && entry.id == id) r.listed[i] = true;
  }
  for (size_t i = 0; i < r.info.size(); ++i) {
    r.info_observed[i] = true;
    r.info[i] = client.GetConfigInfo(ConfigType::kSoftware, PresenceDiagnosticQueryId(i), 5000);
    if (!usable(r.info[i].status)) { r.stopped_on_transport_failure = true; break; }
  }
  return r; // no retry; each independent read result retained, including failures
}
int PresenceQcciGuard::SendRawMsgSync(QmiClientHandle h, unsigned int msg,
    const void* request, unsigned int size, void* response, unsigned int capacity,
    unsigned int* received, unsigned int timeout) {
  if (!request || !size || size > 2048) return -1;
  const auto* data = static_cast<const uint8_t*>(request);
  const auto token = PeekIndicationToken(data, size);
  if (!token) return -1;
  const std::vector<uint8_t> bytes(data, data + size);
  bool allowed = false;
  if (msg == kMsgGetSelectedConfig)
    allowed = bytes == BuildGetSelectedConfigRequest(*token, ConfigType::kSoftware);
  else if (msg == kMsgListConfigs)
    allowed = bytes == BuildListConfigsRequest(*token, ConfigType::kSoftware);
  else if (msg == kMsgGetConfigInfo)
    for (size_t i = 0; i < 5; ++i)
      allowed = allowed || bytes == BuildGetConfigInfoRequest(*token,
          ConfigType::kSoftware, PresenceDiagnosticQueryId(i));
  if (!allowed) return -1;
  return inner_.SendRawMsgSync(h,msg,request,size,response,capacity,received,timeout);
}
}
