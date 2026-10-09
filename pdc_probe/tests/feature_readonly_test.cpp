#include "feature_readonly.h"
#include <gtest/gtest.h>
#include <cstring>
using namespace guacamole_pdc;
namespace {
std::vector<uint8_t> Indication() {
  return {1,2,0,0,0,0x10,4,0,1,0,0,0,0x11,4,0,3,0,0,0,
          0x12,4,0,40,0,0,0,0x13,1,0,1,0x14,4,0,1,0,0,0};
}
class FeatureShim : public IQcciShim {
 public:
  int loads=0, inits=0, sends=0, releases=0, send_result=0;
  bool deliver=true;
  unsigned int message=kMsgGetFeature;
  std::vector<uint8_t> indication=Indication(), ack{2,4,0,0,0,0,0}, request;
  QmiIndicationCallback callback=nullptr;
  void* context=nullptr;
  bool Load(std::string*) override { ++loads;return true; }
  QmiServiceObject PdcGetServiceObject(int32_t, int32_t, int32_t) override { return this; }
  int ClientInitInstance(QmiServiceObject, QmiIndicationCallback cb, void* data,
      uint32_t, QmiClientHandle* h) override {
    ++inits;callback=cb;context=data;*h=this;return 0;
  }
  int ClientRelease(QmiClientHandle) override { ++releases;return 0; }
  int SendRawMsgSync(QmiClientHandle, unsigned int msg, const void* bytes,
      unsigned int n, void* response, unsigned int capacity, unsigned int* received,
      unsigned int) override {
    ++sends;EXPECT_EQ(msg,kMsgGetFeature);
    const auto* p=static_cast<const uint8_t*>(bytes);request.assign(p,p+n);
    if (ack.size()<=capacity) std::memcpy(response,ack.data(),ack.size());
    *received=ack.size();
    if (deliver && callback) callback(this,message,indication.data(),indication.size(),context);
    return send_result;
  }
};
FeatureReadOutcome Query(FeatureShim& shim, FeatureSlot slot=FeatureSlot::kSlot0) {
  FeatureReadQcciGuard guard(shim,slot);
  FeatureReadClient client(guard);
  return client.Read(slot,0); // immediate fake callback, no wall-clock test delay
}
int Send(FeatureReadQcciGuard& guard, unsigned int msg, const std::vector<uint8_t>& b) {
  uint8_t reply[64]{}; unsigned int n=0;
  return guard.SendRawMsgSync(nullptr,msg,b.data(),b.size(),reply,sizeof(reply),&n,0);
}
}
TEST(GetFeatureWire, ExactSlotEncodingAndNoArbitraryCli) {
  EXPECT_EQ(BuildGetFeatureRequest(FeatureSlot::kSlot0,0x12345678),
      (std::vector<uint8_t>{1,4,0,0,0,0,0,0x10,4,0,0x78,0x56,0x34,0x12}));
  EXPECT_EQ(BuildGetFeatureRequest(FeatureSlot::kSlot1,1),
      (std::vector<uint8_t>{1,4,0,1,0,0,0,0x10,4,0,1,0,0,0}));
  EXPECT_TRUE(BuildGetFeatureRequest(static_cast<FeatureSlot>(2),1).empty());
  EXPECT_TRUE(BuildGetFeatureRequest(FeatureSlot::kSlot0,0).empty());
  EXPECT_EQ(ParseFeatureReadCommand({"get-auto-feature-readonly","0"}),FeatureSlot::kSlot0);
  EXPECT_EQ(ParseFeatureReadCommand({"get-auto-feature-readonly","1"}),FeatureSlot::kSlot1);
  for (const auto& v : {"2","-1","01","0x0",""})
    EXPECT_FALSE(ParseFeatureReadCommand({"get-auto-feature-readonly",v}));
  EXPECT_FALSE(ParseFeatureReadCommand({"get-auto-feature-readonly","0","extra"}));
}
TEST(GetFeatureWire, ParseEnumsPresenceAndLocalExtensions) {
  auto b=Indication(); const auto r=ParseGetFeatureIndication(b,1);
  ASSERT_TRUE(r.valid);EXPECT_EQ(r.selection_mode,3u);EXPECT_EQ(r.selection_carrier,40u);
  EXPECT_EQ(r.flex_mapping,true);EXPECT_EQ(r.refresh_mode,1u);
  b.insert(b.end(),{0x15,4,0,1,2,3,4,0x19,2,0,0,0});
  EXPECT_EQ(ParseGetFeatureIndication(b,1).extension_count,2u);
  auto minimal=ParseGetFeatureIndication({1,2,0,0,0,0x10,4,0,1,0,0,0},1);
  EXPECT_TRUE(minimal.valid);EXPECT_FALSE(minimal.selection_mode);
  EXPECT_EQ(ClassifyAutoFeature(minimal.selection_mode,minimal.flex_mapping),AutoFeatureState::kUnknown);
}
TEST(GetFeatureWire, MalformedWrongTokenDuplicatesAndBooleanFailClosed) {
  auto b=Indication();b.pop_back();EXPECT_FALSE(ParseGetFeatureIndication(b,1).valid);
  EXPECT_FALSE(ParseGetFeatureIndication(Indication(),2).valid);
  b=Indication();b.insert(b.end(),{0x11,4,0,3,0,0,0});
  EXPECT_FALSE(ParseGetFeatureIndication(b,1).valid);
  EXPECT_FALSE(ParseGetFeatureIndication({1,2,0,0,0},1).valid);
  b=Indication();b[29]=2; // flexible mapping byte (TLV13 payload)
  EXPECT_FALSE(ParseGetFeatureIndication(b,1).valid);
  b=Indication();b.insert(b.end(),{0x7f,0,0});
  EXPECT_FALSE(ParseGetFeatureIndication(b,1).valid);
}
TEST(GetFeatureClient, RealCallbackRoutingBeforeAckAndRelease) {
  FeatureShim shim;const auto r=Query(shim,FeatureSlot::kSlot1);
  EXPECT_EQ(r.status,ProbeStatus::kOk);EXPECT_EQ(shim.sends,1);EXPECT_EQ(shim.releases,1);
  EXPECT_EQ(shim.request,BuildGetFeatureRequest(FeatureSlot::kSlot1,1));
}
TEST(GetFeatureClient, UnrelatedIndicationAndWrongTokenCannotComplete) {
  FeatureShim a;a.message=0x2e;EXPECT_EQ(Query(a).status,ProbeStatus::kTimeout);EXPECT_EQ(a.sends,1);
  FeatureShim b;b.indication[8]=2;EXPECT_EQ(Query(b).status,ProbeStatus::kTimeout);EXPECT_EQ(b.sends,1);
  FeatureShim c;c.deliver=false;EXPECT_EQ(Query(c).status,ProbeStatus::kTimeout);EXPECT_EQ(c.sends,1);
}
TEST(GetFeatureClient, SyncRejectionIsNotAsyncSuccess) {
  FeatureShim shim;shim.ack={2,4,0,1,0,41,0};
  const auto r=Query(shim);EXPECT_EQ(r.status,ProbeStatus::kRequestRejected);
  EXPECT_EQ(r.sync_error,41);EXPECT_EQ(shim.sends,1);
}
TEST(GetFeatureClient, IndicationRejectionAndMalformedAreDistinct) {
  FeatureShim shim;shim.indication[3]=41;
  auto r=Query(shim);EXPECT_EQ(r.status,ProbeStatus::kIndicationRejected);EXPECT_EQ(r.data.error,41);
  FeatureShim bad;bad.indication[29]=2;
  EXPECT_EQ(Query(bad).status,ProbeStatus::kMalformedResponse);
  FeatureShim bad_ack;bad_ack.ack={2,4,0};
  EXPECT_EQ(Query(bad_ack).status,ProbeStatus::kMalformedResponse);
}
TEST(GetFeatureClient, FailureNoRetryAndInvalidSlotBeforeInit) {
  FeatureShim shim;shim.send_result=-1;
  EXPECT_EQ(Query(shim).status,ProbeStatus::kRequestSendFailed);EXPECT_EQ(shim.sends,1);
  FeatureShim invalid;EXPECT_EQ(Query(invalid,static_cast<FeatureSlot>(2)).status,ProbeStatus::kMalformedResponse);
  EXPECT_EQ(invalid.loads,0);EXPECT_EQ(invalid.inits,0);EXPECT_EQ(invalid.sends,0);
}
TEST(GetFeatureClient, CannotReuseOneQueryClient) {
  FeatureShim shim;FeatureReadQcciGuard guard(shim,FeatureSlot::kSlot0);
  FeatureReadClient client(guard);
  EXPECT_EQ(client.Read(FeatureSlot::kSlot0,0).status,ProbeStatus::kOk);
  EXPECT_EQ(client.Read(FeatureSlot::kSlot0,0).status,ProbeStatus::kMalformedResponse);
  EXPECT_EQ(shim.sends,1);EXPECT_EQ(shim.inits,1);
}
TEST(GetFeatureGuard, NoMutationWrongSlotExtraFieldOrRetry) {
  FeatureShim shim;FeatureReadQcciGuard guard(shim,FeatureSlot::kSlot0);
  auto b=BuildGetFeatureRequest(FeatureSlot::kSlot0,1);
  for (unsigned msg : {0x23u,0x25u,0x26u,0x27u,0x2bu,0x2eu}) EXPECT_EQ(Send(guard,msg,b),-1);
  EXPECT_EQ(Send(guard,kMsgGetFeature,BuildGetFeatureRequest(FeatureSlot::kSlot1,1)),-1);
  auto extra=b;extra.push_back(0);EXPECT_EQ(Send(guard,kMsgGetFeature,extra),-1);
  EXPECT_EQ(shim.sends,0);
  EXPECT_EQ(Send(guard,kMsgGetFeature,b),0);EXPECT_EQ(shim.sends,1);
  EXPECT_EQ(Send(guard,kMsgGetFeature,b),-1);EXPECT_EQ(shim.sends,1);
}
