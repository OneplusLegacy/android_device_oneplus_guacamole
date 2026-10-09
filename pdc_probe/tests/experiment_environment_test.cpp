#include "experiment_environment.h"
#include "pdc_client.h"
#include <gtest/gtest.h>
#include <map>
#include <vector>

// Compile and exercise the actual disabled main branches, without Android
// properties or vendor QCCI linkage. Never replace these stubs with a mock.
#undef GUACAMOLE_MAPPING_DEBUG
#define main GuacamoleMappingDisabledMain
#include "../src/mapping_main.cpp"
#undef main
#undef GUACAMOLE_CARRIER_DEBUG
#define main GuacamoleCarrierDisabledMain
#include "../src/carrier_main.cpp"
#undef main

namespace {
using namespace guacamole_pdc;
class CountingQcci final : public IQcciShim {
 public:
  int calls = 0;
  bool Load(std::string*) override { ++calls; return false; }
  QmiServiceObject PdcGetServiceObject(int32_t, int32_t, int32_t) override { ++calls; return nullptr; }
  int ClientInitInstance(QmiServiceObject, QmiIndicationCallback, void*, uint32_t, QmiClientHandle*) override { ++calls; return -1; }
  int ClientRelease(QmiClientHandle) override { ++calls; return 0; }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override { ++calls; return -1; }
};
struct Environment {
  uint32_t uid = 0;
  int entries = 0;
  std::map<std::string, std::string> properties{
      {"ro.system.build.type", "userdebug"}, {"ro.vendor.build.type", "userdebug"},
      {"ro.debuggable", "0"}, {"ro.build.type", "user"}};
  std::vector<std::string> lookups;
  CountingQcci qcci;
  int Run() {
    return RunGuacamoleManualExperiment([&] { return uid; },
        [&](const std::string& key) { lookups.push_back(key); return properties[key]; },
        [&] {
          ++entries;
          PdcClient client(qcci);
          std::string log;
          client.Init({{1,15,6}}, &log); // fake transport; no vendor library
          return 17; // preserve the entry's exit code
        });
  }
};
TEST(ExperimentEnvironment, ActualCompileDisabledEntryPointsAlwaysBlocked) {
  EXPECT_EQ(GuacamoleMappingDisabledMain(), 4);
  EXPECT_EQ(GuacamoleCarrierDisabledMain(), 4);
  // These compiled branches have no UID/property/transport dependencies at all.
}
TEST(ExperimentEnvironment, NonRootBlocksBeforePropertiesAndQcci) {
  for (uint32_t uid : {1u, 2000u, 2909u}) {
    Environment e; e.uid = uid;
    EXPECT_EQ(e.Run(), 4); EXPECT_EQ(e.entries, 0); EXPECT_EQ(e.qcci.calls, 0);
    EXPECT_TRUE(e.lookups.empty());
  }
}
TEST(ExperimentEnvironment, SystemUserBlockedBeforeQcci) {
  Environment e; e.properties["ro.system.build.type"] = "user";
  EXPECT_EQ(e.Run(), 4); EXPECT_EQ(e.entries, 0); EXPECT_EQ(e.qcci.calls, 0);
}
TEST(ExperimentEnvironment, VendorUserBlockedBeforeQcci) {
  Environment e; e.properties["ro.vendor.build.type"] = "user";
  EXPECT_EQ(e.Run(), 4); EXPECT_EQ(e.entries, 0); EXPECT_EQ(e.qcci.calls, 0);
}
TEST(ExperimentEnvironment, RootUserdebugPairAllowed) {
  Environment e;
  EXPECT_EQ(e.Run(), 17); EXPECT_EQ(e.entries, 1); EXPECT_GT(e.qcci.calls, 0);
}
TEST(ExperimentEnvironment, RootEngPairAllowed) {
  Environment e;
  e.properties["ro.system.build.type"] = e.properties["ro.vendor.build.type"] = "eng";
  EXPECT_EQ(e.Run(), 17); EXPECT_EQ(e.entries, 1);
}
TEST(ExperimentEnvironment, MixedEngUserdebugPairAllowed) {
  for (const auto& pair : {std::make_pair("eng", "userdebug"), std::make_pair("userdebug", "eng")}) {
    Environment e; e.properties["ro.system.build.type"] = pair.first;
    e.properties["ro.vendor.build.type"] = pair.second;
    EXPECT_EQ(e.Run(), 17);
  }
}
TEST(ExperimentEnvironment, DebuggableZeroAndTopLevelUserNotConsulted) {
  Environment e;
  EXPECT_EQ(e.properties["ro.debuggable"], "0"); EXPECT_EQ(e.properties["ro.build.type"], "user");
  EXPECT_EQ(e.Run(), 17);
  EXPECT_EQ(e.lookups, (std::vector<std::string>{"ro.system.build.type", "ro.vendor.build.type"}));
}
TEST(ExperimentEnvironment, MissingUnknownOrNonexactBuildTypeBlocked) {
  for (const auto& key : {"ro.system.build.type", "ro.vendor.build.type"}) {
    for (const auto& value : {"", "unknown", "USERDEBUG", "userdebug ", "eng\n"}) {
      Environment e; e.properties[key] = value;
      EXPECT_EQ(e.Run(), 4); EXPECT_EQ(e.entries, 0); EXPECT_EQ(e.qcci.calls, 0);
    }
  }
}
TEST(ExperimentEnvironment, DebuggableOneCannotOverrideUserPartition) {
  Environment e; e.properties["ro.debuggable"] = "1";
  e.properties["ro.vendor.build.type"] = "user";
  EXPECT_EQ(e.Run(), 4); EXPECT_EQ(e.qcci.calls, 0);
}
}
