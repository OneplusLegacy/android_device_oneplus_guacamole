#include "auto_feature_set.h"
#include "auto_slot0_test_support.h"
#include <gtest/gtest.h>
#include <cstring>
using namespace guacamole_pdc;
using namespace guacamole_pdc::auto_test;
namespace {
class Shim : public IQcciShim {
 public:
  int sends=0,inits=0,releases=0,rc=0;
  bool deliver=true,duplicate=false;
  unsigned int msg=kObservedAutoMessage;
  std::vector<uint8_t> ack{2,4,0,0,0,0,0},ind{1,2,0,0,0},request;
  QmiIndicationCallback callback=nullptr;void* context=nullptr;
  bool Load(std::string*) override {return true;}
  QmiServiceObject PdcGetServiceObject(int32_t,int32_t,int32_t) override {return this;}
  int ClientInitInstance(QmiServiceObject,QmiIndicationCallback cb,void* data,uint32_t,QmiClientHandle* h) override {
    ++inits;callback=cb;context=data;*h=this;return 0;
  }
  int ClientRelease(QmiClientHandle) override {++releases;return 0;}
  int SendRawMsgSync(QmiClientHandle,unsigned int message,const void* p,unsigned int n,
      void* reply,unsigned int capacity,unsigned int* got,unsigned int) override {
    ++sends;EXPECT_EQ(message,0x2eu);
    const auto* b=static_cast<const uint8_t*>(p);request.assign(b,b+n);
    if(ack.size()<=capacity) std::memcpy(reply,ack.data(),ack.size());
    *got=ack.size();
    if(deliver && callback) {
      callback(this,msg,ind.data(),ind.size(),context);
      if(duplicate)callback(this,msg,ind.data(),ind.size(),context);
    }
    return rc;
  }
};
AutoFeatureSetResult RunAutoSetClientForTest(Shim& shim) {
  State state;state.durable=true;AutoFeatureSetQcciGuard guard(shim);
  EXPECT_TRUE(guard.Arm(Pre(),kAutoSlot0Confirmation,state));
  AutoFeatureSetClient client(guard);return client.EnableSlot0(0);
}
int Send(AutoFeatureSetQcciGuard& guard,unsigned int msg,const std::vector<uint8_t>& b) {
  uint8_t reply[64]{};unsigned int n=0;
  return guard.SendRawMsgSync(nullptr,msg,b.data(),b.size(),reply,sizeof(reply),&n,0);
}
}
TEST(AutoSetGuard, ExactBytesOnePermitConsumedEvenOnSendFailure) {
  Shim shim;shim.rc=-1;State state;state.durable=true;AutoFeatureSetQcciGuard guard(shim);
  ASSERT_TRUE(guard.Arm(Pre(),kAutoSlot0Confirmation,state));
  const auto expected=std::vector<uint8_t>{1,4,0,0,0,0,0,0x11,4,0,3,0,0,0,0x13,1,0,1};
  EXPECT_EQ(BuildAutoFeatureRequest(FeatureSlot::kSlot0,true),expected);
  Send(guard,0x2e,expected);Send(guard,0x2e,expected);
  EXPECT_EQ(shim.sends,1);EXPECT_TRUE(guard.Attempted());EXPECT_FALSE(guard.Armed());
  EXPECT_FALSE(guard.Arm(Pre(),kAutoSlot0Confirmation,state));
}
TEST(AutoSetGuard, NoOtherMessageSlotDisableTokenCarrierRefreshOrRawRequest) {
  Shim shim;State state;state.durable=true;AutoFeatureSetQcciGuard guard(shim);
  ASSERT_TRUE(guard.Arm(Pre(),kAutoSlot0Confirmation,state));
  const auto bytes=BuildAutoFeatureRequest(FeatureSlot::kSlot0,true);
  for(unsigned msg:{0x23u,0x25u,0x26u,0x27u,0x2bu,0x2du}) EXPECT_EQ(Send(guard,msg,bytes),-1);
  EXPECT_EQ(Send(guard,0x2e,BuildAutoFeatureRequest(FeatureSlot::kSlot1,true)),-1);
  EXPECT_EQ(Send(guard,0x2e,BuildAutoFeatureRequest(FeatureSlot::kSlot0,false)),-1);
  for(uint8_t tlv:{0x10,0x12,0x14}) {
    auto bad=bytes;bad.insert(bad.end(),{tlv,4,0,0,0,0,0});EXPECT_EQ(Send(guard,0x2e,bad),-1);
  }
  auto bad=bytes;bad[0]=0x7f;EXPECT_EQ(Send(guard,0x2e,bad),-1);
  EXPECT_EQ(shim.sends,0);EXPECT_FALSE(guard.Attempted());
}
TEST(AutoSetGuard, MissingCheckpointWrongConfirmationAndBadPreflightCannotArm) {
  for(int bad=0;bad<3;++bad) {
    Shim shim;State state;state.durable=bad!=0;auto pre=Pre();
    if(bad==2)pre.selected.data.pending_id=CarrierId(CarrierTarget::kSkt);
    AutoFeatureSetQcciGuard guard(shim);
    EXPECT_FALSE(guard.Arm(pre,bad==1?"wrong":kAutoSlot0Confirmation,state));
    AutoFeatureSetClient client(guard);EXPECT_FALSE(client.EnableSlot0(0).mutation_attempted);
    EXPECT_EQ(shim.sends,0);EXPECT_EQ(shim.inits,0);
  }
}
TEST(AutoSetClient, EarlyIndicationPlusSyncSuccessAndRelease) {
  Shim shim;const auto r=RunAutoSetClientForTest(shim);EXPECT_EQ(r.status,ProbeStatus::kOk);
  EXPECT_TRUE(r.sync.valid);EXPECT_TRUE(r.indication.valid);EXPECT_FALSE(r.indication.token);
  EXPECT_TRUE(r.mutation_attempted);EXPECT_FALSE(r.ambiguous);
  EXPECT_EQ(shim.sends,1);EXPECT_EQ(shim.releases,1);
}
TEST(AutoSetClient, SendFailureIsAmbiguousDespitePossibleEarlyIndication) {
  Shim shim;shim.rc=-1;const auto r=RunAutoSetClientForTest(shim);
  EXPECT_EQ(r.status,ProbeStatus::kRequestSendFailed);EXPECT_TRUE(r.ambiguous);
  EXPECT_TRUE(r.indication.valid);EXPECT_EQ(shim.sends,1);
}
TEST(AutoSetClient, SyncRejectionCannotBeOverriddenBySuccessIndication) {
  Shim shim;shim.ack={2,4,0,1,0,41,0};const auto r=RunAutoSetClientForTest(shim);
  EXPECT_EQ(r.status,ProbeStatus::kRequestRejected);EXPECT_EQ(r.sync.error,41);
  EXPECT_TRUE(r.mutation_attempted);EXPECT_FALSE(r.ambiguous);EXPECT_EQ(shim.sends,1);
}
TEST(AutoSetClient, AsyncRejectionDistinctFromTimeout) {
  Shim shim;shim.ind={1,2,0,41,0};const auto r=RunAutoSetClientForTest(shim);
  EXPECT_EQ(r.status,ProbeStatus::kIndicationRejected);EXPECT_EQ(r.indication.error,41);
  EXPECT_EQ(shim.sends,1);EXPECT_FALSE(r.ambiguous);
}
TEST(AutoSetClient, MissingOrUnrelatedIndicationIsAmbiguousNoRetry) {
  for(int kind=0;kind<2;++kind) {
    Shim shim;if(kind==0)shim.deliver=false;else shim.msg=0x2d;
    const auto r=RunAutoSetClientForTest(shim);EXPECT_EQ(r.status,ProbeStatus::kTimeout);
    EXPECT_TRUE(r.ambiguous);EXPECT_EQ(shim.sends,1);
  }
}
TEST(AutoSetClient, MalformedAckIndicationDuplicateOrOversizeIsAmbiguous) {
  for(int kind=0;kind<5;++kind) {
    Shim shim;
    if(kind==0)shim.ack={2,4,0};
    if(kind==1)shim.ind={1,2,0,0};
    if(kind==2)shim.duplicate=true;
    if(kind==3)shim.ind.resize(13);
    if(kind==4)shim.ind={1,2,0,0,0,1,2,0,0,0};
    const auto r=RunAutoSetClientForTest(shim);EXPECT_EQ(r.status,ProbeStatus::kMalformedResponse);
    EXPECT_TRUE(r.ambiguous);EXPECT_EQ(shim.sends,1);
  }
}
TEST(AutoSetClient, SameObjectCannotInitializeOrSendTwice) {
  Shim shim;State state;state.durable=true;AutoFeatureSetQcciGuard guard(shim);
  ASSERT_TRUE(guard.Arm(Pre(),kAutoSlot0Confirmation,state));AutoFeatureSetClient client(guard);
  EXPECT_EQ(client.EnableSlot0(0).status,ProbeStatus::kOk);
  EXPECT_FALSE(client.EnableSlot0(0).mutation_attempted);EXPECT_EQ(shim.sends,1);EXPECT_EQ(shim.inits,1);
}
