#include "experiment_context.h"
#include "experiment_environment.h"
#include "auto_slot0_test_support.h"
#include <gtest/gtest.h>

using namespace guacamole_pdc;
namespace {
SelfContextReader Context(const std::string& value) {
  return [value](std::string* out) { *out=value; return true; };
}
}
TEST(ExperimentContext, ExactContextPassesOnce) {
  int calls=0;
  EXPECT_EQ(RunWithGuacamoleProbeContext(Context(kGuacamoleProbeContext),[&]{++calls;return 10;}),10);
  EXPECT_EQ(calls,1);
}
TEST(ExperimentContext, SuAndShellBlockedBeforeJournalOrQcci) {
  for (const char* value : {"u:r:su:s0", "u:r:shell:s0"}) {
    auto_test::State state; auto_test::Ops ops;
    EXPECT_EQ(RunWithGuacamoleProbeContext(Context(value),[&]{
      RunAutoSlot0Experiment(ops,state,auto_test::Command()); return 10;
    }),4);
    EXPECT_EQ(state.acquires,0); EXPECT_EQ(state.records,0); EXPECT_TRUE(ops.order.empty());
  }
}
TEST(ExperimentContext, EmptyMalformedAndNonExactContextsBlocked) {
  for (const char* value : {"", "guacamole_pdc_probe", "u:r:guacamole_pdc_probe:s0:c1",
                           "u:r:guacamole_pdc_probe:s0\n", "u:r:guacamole_pdc_probe_other:s0"}) {
    int calls=0;
    EXPECT_EQ(RunWithGuacamoleProbeContext(Context(value),[&]{++calls;return 0;}),4);
    EXPECT_EQ(calls,0);
  }
  std::string embedded=kGuacamoleProbeContext; embedded.push_back('\0'); embedded+='x';
  EXPECT_EQ(RunWithGuacamoleProbeContext(Context(embedded),[]{ADD_FAILURE();return 0;}),4);
}
TEST(ExperimentContext, ReadFailureOrMissingProviderBlocksEvenWithExpectedBytes) {
  const SelfContextReader failure=[](std::string* out){*out=kGuacamoleProbeContext;return false;};
  EXPECT_EQ(RunWithGuacamoleProbeContext(failure,[]{ADD_FAILURE();return 0;}),4);
  EXPECT_EQ(RunWithGuacamoleProbeContext({},[]{ADD_FAILURE();return 0;}),4);
}
TEST(ExperimentContext, ContextReadPrecedesJournalCheckpointAndOperations) {
  auto_test::State state; auto_test::Ops ops; int reads=0;
  const int rc=RunWithGuacamoleProbeContext([&](std::string* out){
    ++reads; EXPECT_EQ(state.acquires,0); EXPECT_EQ(state.records,0); EXPECT_TRUE(ops.order.empty());
    *out=kGuacamoleProbeContext;return true;
  },[&]{
    EXPECT_EQ(reads,1);
    const auto result=RunAutoSlot0Experiment(ops,state,auto_test::Command());
    EXPECT_TRUE(result.checkpoint_durable); return 10;
  });
  EXPECT_EQ(rc,10); EXPECT_EQ(state.records,1); EXPECT_EQ(ops.sets,1);
}
TEST(ExperimentContext, ReadOnlyDiagnosticOnlyCallsContextProvider) {
  int reads=0;
  EXPECT_EQ(ReportGuacamoleSelfContext(0,[&](std::string* out){++reads;*out=kGuacamoleProbeContext;return true;}),0);
  EXPECT_EQ(reads,1);
  EXPECT_EQ(ReportGuacamoleSelfContext(0,Context("u:r:su:s0")),4);
  EXPECT_EQ(ReportGuacamoleSelfContext(0,[](std::string*){return false;}),4);
  // Report has no operation continuation, QCCI, or journal reference.
}
TEST(ExperimentContext, RootAndBothDebugBuildGatesRemainOuterRequirements) {
  for (int bad=0;bad<3;++bad) {
    int reads=0,entries=0;
    const int rc=RunGuacamoleManualExperiment([&]{return bad==0?2000u:0u;},
      [&](const std::string& key){return ((bad==1 && key=="ro.system.build.type") ||
        (bad==2 && key=="ro.vendor.build.type"))?"user":"userdebug";},
      [&]{return RunWithGuacamoleProbeContext([&](std::string* out){++reads;*out=kGuacamoleProbeContext;return true;},
        [&]{++entries;return 10;});});
    EXPECT_EQ(rc,4); EXPECT_EQ(reads,0); EXPECT_EQ(entries,0);
  }
}
