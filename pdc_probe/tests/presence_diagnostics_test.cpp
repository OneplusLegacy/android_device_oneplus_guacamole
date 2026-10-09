#include "presence_diagnostics.h"
#include <gtest/gtest.h>
namespace {
using namespace guacamole_pdc;
class ReadClient final : public IPdcClient {
 public:
  int mutations = 0, selected = 0, lists = 0;
  std::vector<std::vector<uint8_t>> ids;
  ReadClient() { listing.status=ProbeStatus::kOk; listing.data.ok=true; info.status=ProbeStatus::kOk; info.data.ok=true; }
  ListConfigsOutcome listing;
  GetConfigInfoOutcome info;
  ListConfigsOutcome ListConfigs(uint32_t) override { ++lists; return listing; }
  GetSelectedConfigOutcome GetSelectedConfig(ConfigType t, uint32_t) override {
    EXPECT_EQ(t,ConfigType::kSoftware); ++selected; GetSelectedConfigOutcome o; o.status=ProbeStatus::kOk; o.data.ok=true; return o;
  }
  GetConfigInfoOutcome GetConfigInfo(ConfigType t, const std::vector<uint8_t>& id, uint32_t) override {
    EXPECT_EQ(t,ConfigType::kSoftware); ids.push_back(id); return info;
  }
  LoadConfigChunkOutcome LoadConfigChunk(ConfigType,const std::vector<uint8_t>&,uint32_t,
      const uint8_t*,size_t,uint32_t) override { ++mutations; return {}; }
  SetSelectedConfigOutcome SetSelectedConfig(ConfigType,const std::vector<uint8_t>&,uint32_t) override {
    ++mutations; return {};
  }
};
TEST(PresenceDiagnostics, ExactFixedCommandOnly) {
  EXPECT_TRUE(IsPresenceDiagnosticsCommand({"diagnose-presence"}));
  EXPECT_FALSE(IsPresenceDiagnosticsCommand({"diagnose-presence","001122"}));
  EXPECT_FALSE(IsPresenceDiagnosticsCommand({"diagnose-presence","--id","abc"}));
  EXPECT_FALSE(IsPresenceDiagnosticsCommand({}));
}
TEST(PresenceDiagnostics, FixedIdsAndNoMutationOrRetry) {
  ReadClient c;
  c.info.status = ProbeStatus::kIndicationRejected; c.info.data.ok=true; c.info.data.error_status=41;
  const auto r = DiagnosePresence(c);
  EXPECT_EQ(c.selected,1); EXPECT_EQ(c.lists,1); EXPECT_EQ(c.mutations,0);
  ASSERT_EQ(c.ids.size(),5u);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(c.ids[i],PresenceDiagnosticQueryId(i));
    EXPECT_EQ(r.info[i].status,ProbeStatus::kIndicationRejected);
  }
  EXPECT_EQ(c.ids[2],CarrierId(CarrierTarget::kKt));
  EXPECT_EQ(c.ids[3],CarrierId(CarrierTarget::kLgu));
  EXPECT_EQ(c.ids[4],(std::vector<uint8_t>{0x47,0x55,0x41,0x43,0x41,0x4d,0x4f,0x4c,0x45,0x2d,
      0x50,0x44,0x43,0x2d,0x44,0x49,0x41,0x47,0x30,0x31}));
  EXPECT_TRUE(PresenceDiagnosticQueryId(5).empty());
}
TEST(PresenceDiagnostics, TimeoutAndMalformedStopWithoutRetry) {
  for (auto status : {ProbeStatus::kTimeout, ProbeStatus::kMalformedResponse,
      ProbeStatus::kRequestSendFailed}) {
    ReadClient c; c.info.status=status;
    const auto r=DiagnosePresence(c);
    EXPECT_TRUE(r.stopped_on_transport_failure);
    EXPECT_EQ(c.ids.size(),1u); EXPECT_EQ(c.mutations,0);
    EXPECT_TRUE(r.info_observed[0]); EXPECT_FALSE(r.info_observed[1]);
  }
}
TEST(PresenceDiagnostics, Async50OnlyAbsentAnd41Unknown) {
  GetConfigInfoOutcome o; o.data.ok=true; o.status=ProbeStatus::kIndicationRejected;
  o.data.error_status=41;
  EXPECT_EQ(CarrierPresence(o),ConfigPresence::kUnknown);
  o.data.error_status=50;
  EXPECT_EQ(CarrierPresence(o),ConfigPresence::kAbsent);
  o.status=ProbeStatus::kRequestRejected; o.data.error_status=1; o.data.error_code=50;
  EXPECT_EQ(CarrierPresence(o),ConfigPresence::kUnknown);
  o.status=ProbeStatus::kMalformedResponse;
  EXPECT_EQ(CarrierPresence(o),ConfigPresence::kUnknown);
}
TEST(PresenceDiagnostics, OmissionNeverImpliesAbsenceAndSyntheticMayExist) {
  ReadClient c; c.info.status=ProbeStatus::kOk; c.info.data.ok=true;
  const auto r=DiagnosePresence(c);
  for (size_t i=0;i<5;++i) {
    EXPECT_FALSE(r.listed[i]); EXPECT_EQ(CarrierPresence(r.info[i]),ConfigPresence::kExists);
  }
}
TEST(PresenceDiagnostics, RawMembershipChecksTypeAndExactId) {
  ReadClient c;
  c.listing.data.configs={{ConfigType::kSoftware,CarrierId(CarrierTarget::kKt)},
      {ConfigType::kPlatform,CarrierId(CarrierTarget::kLgu)}};
  const auto r=DiagnosePresence(c);
  EXPECT_TRUE(r.listed[2]); EXPECT_FALSE(r.listed[3]);
}
class CountWire final : public IQcciShim {
 public:
  int sends=0;
  bool Load(std::string*) override { return false; }
  QmiServiceObject PdcGetServiceObject(int32_t,int32_t,int32_t) override { return nullptr; }
  int ClientInitInstance(QmiServiceObject,QmiIndicationCallback,void*,uint32_t,QmiClientHandle*) override { return -1; }
  int ClientRelease(QmiClientHandle) override { return 0; }
  int SendRawMsgSync(QmiClientHandle,unsigned int,const void*,unsigned int,void*,unsigned int,unsigned int*,unsigned int) override {
    ++sends; return 0;
  }
};
int Send(PresenceQcciGuard& g,unsigned int msg,const std::vector<uint8_t>& b) {
  return g.SendRawMsgSync(nullptr,msg,b.data(),b.size(),nullptr,0,nullptr,5000);
}
TEST(PresenceDiagnostics, ReadOnlyTransportExactAllowlist) {
  CountWire w; PresenceQcciGuard g(w);
  EXPECT_EQ(Send(g,kMsgGetSelectedConfig,BuildGetSelectedConfigRequest(1,ConfigType::kSoftware)),0);
  EXPECT_EQ(Send(g,kMsgListConfigs,BuildListConfigsRequest(2,ConfigType::kSoftware)),0);
  for (size_t i=0;i<5;++i)
    EXPECT_EQ(Send(g,kMsgGetConfigInfo,BuildGetConfigInfoRequest(3,ConfigType::kSoftware,PresenceDiagnosticQueryId(i))),0);
  EXPECT_EQ(w.sends,7);
  EXPECT_EQ(Send(g,kMsgGetConfigInfo,BuildGetConfigInfoRequest(4,ConfigType::kSoftware,{1,2,3})),-1);
  EXPECT_EQ(Send(g,kMsgGetConfigInfo,BuildGetConfigInfoRequest(5,ConfigType::kPlatform,PresenceDiagnosticId())),-1);
  auto extra=BuildGetConfigInfoRequest(6,ConfigType::kSoftware,PresenceDiagnosticId());
  extra.push_back(0);
  EXPECT_EQ(Send(g,kMsgGetConfigInfo,extra),-1);
  EXPECT_EQ(w.sends,7);
}
TEST(PresenceDiagnostics, LoadSelectActivateDeleteAllBlockedAtTransport) {
  CountWire w; PresenceQcciGuard g(w);
  const auto id=CarrierId(CarrierTarget::kKt);
  EXPECT_EQ(Send(g,kMsgSetSelectedConfig,BuildSetSelectedConfigRequest(1,ConfigType::kSoftware,id)),-1);
  const uint8_t b=0;
  EXPECT_EQ(Send(g,kMsgLoadConfig,BuildLoadConfigChunkRequest(2,ConfigType::kSoftware,id,1,&b,1)),-1);
  for (unsigned int msg : {0x25u,0x27u,0x29u})
    EXPECT_EQ(Send(g,msg,BuildGetSelectedConfigRequest(3,ConfigType::kSoftware)),-1);
  EXPECT_EQ(w.sends,0);
}
}
