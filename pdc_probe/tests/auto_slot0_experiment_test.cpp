#include "auto_slot0_test_support.h"
#include "experiment_environment.h"
#include <gtest/gtest.h>
using namespace guacamole_pdc;
using namespace guacamole_pdc::auto_test;
TEST(AutoSlot0State, ExactConfirmationAndSlot1Closed) {
  EXPECT_TRUE(IsAutoSlot0Command(Command()));
  EXPECT_TRUE(IsClosedAutoSlot1Command({"enable-auto-selection-slot1-once","--confirm","GUACAMOLE_AUTO_SLOT1_ENABLE_EXPERIMENT_V1"}));
  for(auto command:std::vector<std::vector<std::string>>{{"enable-auto-selection-once","--confirm",kAutoSlot0Confirmation},
      {"enable-auto-selection-slot1-once","--confirm",kAutoSlot0Confirmation},
      {"enable-auto-selection-slot0-once","--confirm","wrong"},
      {"enable-auto-selection-slot0-once","--confirm",kAutoSlot0Confirmation,"extra"}}) {
    Ops ops;State state;const auto r=RunAutoSlot0Experiment(ops,state,command);
    EXPECT_FALSE(r.set.mutation_attempted);EXPECT_EQ(state.acquires,0);EXPECT_TRUE(ops.order.empty());
  }
}
TEST(AutoSlot0State, RootAndDebugEnvironmentFailBeforeAnyClient) {
  for (int kind=0;kind<3;++kind) {
    Ops ops;State state;
    const int rc=RunGuacamoleManualExperiment([&]{return kind==0?2000u:0u;},
        [&](const std::string& key){return (kind==1 && key=="ro.system.build.type") ||
            (kind==2 && key=="ro.vendor.build.type")?"user":"userdebug";},
        [&]{RunAutoSlot0Experiment(ops,state,Command());return 10;});
    EXPECT_EQ(rc,4);EXPECT_EQ(state.acquires,0);EXPECT_TRUE(ops.order.empty());
  }
  // Non-debug main/device-adapter branches are also checked by static review.
}
TEST(AutoSlot0State, UnresolvedStateBeforeReadsAndCheckpointFailureBeforeSet) {
  Ops ops;State state;state.clean=false;
  EXPECT_FALSE(RunAutoSlot0Experiment(ops,state,Command()).set.mutation_attempted);
  EXPECT_TRUE(ops.order.empty());
  Ops other;State torn;torn.record_ok=false;
  EXPECT_FALSE(RunAutoSlot0Experiment(other,torn,Command()).set.mutation_attempted);
  EXPECT_EQ(other.sets,0);EXPECT_EQ(torn.records,1);
}
TEST(AutoSlot0State, ActivePendingAndFeaturePreflightFailClosed) {
  for(int bad=0;bad<6;++bad) {
    Ops ops;State state;
    if(bad==0)ops.pre.selected.data.active_id=CarrierId(CarrierTarget::kSkt);
    if(bad==1)ops.pre.selected.data.pending_id=CarrierId(CarrierTarget::kCommon);
    if(bad==2)ops.pre.slots[0]=auto_test::Feature(3,true);
    if(bad==3)ops.pre.slots[0].data.flex_mapping.reset();
    if(bad==4)ops.pre.slots[1].status=ProbeStatus::kIndicationRejected;
    if(bad==5)ops.pre.slots[0].data.valid=false;
    EXPECT_FALSE(RunAutoSlot0Experiment(ops,state,Command()).set.mutation_attempted);
    EXPECT_EQ(ops.sets,0);EXPECT_EQ(state.records,0);
  }
}
TEST(AutoSlot0State, EveryCarrierRequiresExactRuntimeMetadata) {
  for(size_t i=0;i<4;++i) for(int field=0;field<4;++field) {
    Ops ops;State state;
    if(field==0)ops.pre.info[i].data.has_version=false;
    if(field==1)++ops.pre.info[i].data.version;
    if(field==2)ops.pre.info[i].data.description="different";
    if(field==3)++ops.pre.info[i].data.total_size;
    EXPECT_FALSE(RunAutoSlot0Experiment(ops,state,Command()).set.mutation_attempted);
    EXPECT_EQ(ops.sets,0);EXPECT_EQ(state.records,0);
  }
}
TEST(AutoSlot0State, Slot1ValueObservedNotUsedAsSlot0EnableProxy) {
  Ops ops;State state;ops.pre.slots[1]=auto_test::Feature(3,true);
  const auto r=RunAutoSlot0Experiment(ops,state,Command());
  EXPECT_TRUE(r.set.mutation_attempted);EXPECT_EQ(r.pre.slots[1].data.selection_mode,3u);
  EXPECT_EQ(ops.sets,1);
}
TEST(AutoSlot0State, DurableCheckpointBeforeSetThenPostEachOnce) {
  Ops ops;State state;int printed=0;
  const auto r=RunAutoSlot0Experiment(ops,state,Command(),[&](const AutoSlot0Report& before){
    ++printed;EXPECT_TRUE(state.PendingDurable());EXPECT_TRUE(before.checkpoint_durable);EXPECT_EQ(ops.sets,0);
  });
  EXPECT_EQ(printed,1);EXPECT_EQ(ops.sets,1);EXPECT_EQ(ops.selected,2);
  EXPECT_EQ(ops.feature_calls,(std::array<int,2>{2,2}));
  EXPECT_EQ(ops.info_calls,(std::array<int,4>{1,1,1,1}));
  EXPECT_EQ(ops.order,(std::vector<std::string>{"selected","slot0","slot1","info","info","info","info","set0","slot0","slot1","selected"}));
  EXPECT_EQ(r.assessment,"AUTO_SLOT0_FEATURE_ENABLE_CONFIRMED");EXPECT_EQ(r.outcome,"USER_REVIEW_REQUIRED");
  EXPECT_TRUE(state.PendingDurable()); // retained even after success; no auto clear
}
TEST(AutoSlot0State, AnyAttemptFailureStillPostObservesWithoutRetry) {
  for(auto status:{ProbeStatus::kRequestSendFailed,ProbeStatus::kTimeout,ProbeStatus::kMalformedResponse,
                  ProbeStatus::kRequestRejected,ProbeStatus::kIndicationRejected}) {
    Ops ops;State state;ops.set.status=status;ops.set.ambiguous=status==ProbeStatus::kTimeout;
    const auto r=RunAutoSlot0Experiment(ops,state,Command());
    EXPECT_EQ(ops.sets,1);EXPECT_EQ(ops.selected,2);EXPECT_EQ(ops.feature_calls,(std::array<int,2>{2,2}));
    EXPECT_TRUE(r.post_observed);EXPECT_EQ(r.outcome,"USER_REVIEW_REQUIRED");
    EXPECT_NE(r.assessment,"AUTO_SLOT0_FEATURE_ENABLE_CONFIRMED");
  }
}
TEST(AutoSlot0State, AcceptanceAloneOrSelectedChangeCannotConfirm) {
  for(int mismatch=0;mismatch<3;++mismatch) {
    Ops ops;State state;
    if(mismatch==0)ops.post[0]=auto_test::Feature();
    if(mismatch==1)ops.post_selected.data.active_id=CarrierId(CarrierTarget::kSkt);
    if(mismatch==2)ops.post_selected.data.pending_id=CarrierId(CarrierTarget::kKt);
    EXPECT_EQ(RunAutoSlot0Experiment(ops,state,Command()).assessment,"PROTOCOL_ACCEPTED_UNEXPECTED_STATE");
    EXPECT_EQ(ops.sets,1);
  }
}
TEST(AutoSlot0State, FailedPostReadsStillAttemptRemainingReadsAndNoRollback) {
  Ops ops;State state;ops.post[0].status=ProbeStatus::kTimeout;ops.post[1].status=ProbeStatus::kTimeout;
  const auto r=RunAutoSlot0Experiment(ops,state,Command());
  EXPECT_EQ(ops.selected,2);EXPECT_EQ(ops.feature_calls,(std::array<int,2>{2,2}));EXPECT_EQ(ops.sets,1);
  EXPECT_EQ(r.assessment,"PROTOCOL_ACCEPTED_UNEXPECTED_STATE");
}
TEST(AutoSlot0State, CannotRetrySamePersistentAttemptEvenIfPostSeemsSuccessful) {
  Ops ops;State state;RunAutoSlot0Experiment(ops,state,Command());
  Ops second;const auto r=RunAutoSlot0Experiment(second,state,Command());
  EXPECT_FALSE(r.set.mutation_attempted);EXPECT_TRUE(second.order.empty());
}
TEST(AutoSlot0State, InitFailureDoesNotClaimMutationAndCheckpointRemains) {
  Ops ops;State state;ops.set={};const auto r=RunAutoSlot0Experiment(ops,state,Command());
  EXPECT_FALSE(r.set.mutation_attempted);EXPECT_FALSE(r.post_observed);EXPECT_TRUE(state.PendingDurable());
}

TEST(AutoSlot0State, B4DeviceGateClosedEvenWithOtherwiseValidPreflight) {
  Ops ops;State state;
  const auto result=RunAutoSlot0DeviceGate([&]{return RunAutoSlot0Experiment(ops,state,Command());});
  EXPECT_FALSE(kDeviceAutoSlot0SendOpen);
  EXPECT_EQ(result.outcome,"AUTO_EXPERIMENT_CLOSED");
  EXPECT_FALSE(result.set.mutation_attempted);
  EXPECT_EQ(state.acquires,0);EXPECT_EQ(state.records,0);EXPECT_TRUE(ops.order.empty());
}
TEST(AutoSlot0State, B4ClosedGateDoesNotConsumeOrClearRetainedPending) {
  Ops ops;State state;state.clean=false;state.durable=true;
  const auto result=RunAutoSlot0DeviceGate([&]{return RunAutoSlot0Experiment(ops,state,Command());});
  EXPECT_EQ(result.outcome,"AUTO_EXPERIMENT_CLOSED");
  EXPECT_TRUE(state.PendingDurable());EXPECT_FALSE(state.clean);
  EXPECT_EQ(state.acquires,0);EXPECT_TRUE(ops.order.empty());
}
