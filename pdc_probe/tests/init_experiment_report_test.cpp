#include "init_experiment_report.h"
#include "auto_slot0_experiment.h"
#include "experiment_context.h"
#include <gtest/gtest.h>
using namespace guacamole_pdc;
TEST(InitExperimentReport, ReadOnlyCommandsAreFixed) {
  const auto context=ParseInitReportCommand({"--init-report","self-context-readonly"});
  ASSERT_TRUE(context); EXPECT_EQ(context->label,"context");
  for (const char* slot : {"0","1"}) {
    const auto r=ParseInitReportCommand({"--init-report","get-auto-feature-readonly",slot});
    ASSERT_TRUE(r); EXPECT_EQ(r->args,(std::vector<std::string>{"get-auto-feature-readonly",slot}));
  }
  ASSERT_TRUE(ParseInitReportCommand({"--init-report","selected"}));
}
TEST(InitExperimentReport, Slot0NeedsExactStrongConfirmation) {
  const auto r=ParseInitReportCommand({"--init-report","enable-auto-selection-slot0-once","--confirm",kAutoSlot0Confirmation});
  ASSERT_TRUE(r); EXPECT_EQ(r->label,"auto0-enable-once"); EXPECT_TRUE(IsAutoSlot0Command(r->args));
  EXPECT_FALSE(ParseInitReportCommand({"--init-report","enable-auto-selection-slot0-once","--confirm","wrong"}));
}
TEST(InitExperimentReport, NoArbitraryCommandPathSlotOrRecursiveWrapper) {
  for (const auto& a:std::vector<std::vector<std::string>>{
      {},{"--init-report"},{"--init-report","--init-report","selected"},
      {"--init-report","/vendor/bin/sh"},{"--init-report","selected","/tmp/log"},
      {"--init-report","get-auto-feature-readonly","2"},
      {"--init-report","enable-auto-selection-slot1-once","--confirm","GUACAMOLE_AUTO_SLOT1_ENABLE_EXPERIMENT_V1"},
      {"--init-report","load-kt-once"},{"--init-report","select-common-once"},
      {"--init-report","activate"},{"--init-report","snapshot-mcfg-readonly"}})
    EXPECT_FALSE(ParseInitReportCommand(a));
}
TEST(InitExperimentReport, InvalidReportCannotInvokeBackendOrOpenDeviceDirectory) {
  int calls=0;
  EXPECT_EQ(RunInitExperimentReport({"../../bad",{"selected"}},[&]{++calls;return 10;}),4);
  EXPECT_EQ(calls,0);
}
TEST(InitExperimentReport, WrongContextBlocksBeforeReportContinuation) {
  int reports=0;
  EXPECT_EQ(RunWithGuacamoleProbeContext([](std::string* c){*c="u:r:su:s0";return true;},
      [&]{++reports;return 10;}),4);
  EXPECT_EQ(reports,0);
}
