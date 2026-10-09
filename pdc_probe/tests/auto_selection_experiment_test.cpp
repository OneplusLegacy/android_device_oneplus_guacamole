#include "auto_selection_experiment.h"
#include <gtest/gtest.h>
using namespace guacamole_pdc;
TEST(AutoExperiment, ExactObservedEnableBytesWithoutInventedToken) {
  EXPECT_EQ(BuildObservedAutoEnableRequest(ObservedAutoSelector::kZero),
      (std::vector<uint8_t>{1,4,0,0,0,0,0,0x11,4,0,3,0,0,0,0x13,1,0,1}));
  EXPECT_EQ(BuildObservedAutoEnableRequest(ObservedAutoSelector::kOne),
      (std::vector<uint8_t>{1,4,0,1,0,0,0,0x11,4,0,3,0,0,0,0x13,1,0,1}));
  EXPECT_TRUE(BuildObservedAutoEnableRequest(static_cast<ObservedAutoSelector>(2)).empty());
}
TEST(AutoExperiment, SyncResultIsNotAsyncCompletion) {
  auto r=ParseObservedAutoSync({2,4,0,0,0,0,0});EXPECT_TRUE(r.valid);EXPECT_EQ(r.result,0);
  r=ParseObservedAutoSync({2,4,0,1,0,41,0});EXPECT_TRUE(r.valid);EXPECT_EQ(r.error,41);
  EXPECT_FALSE(ParseObservedAutoSync({1,2,0,0,0}).valid);
  EXPECT_FALSE(ParseObservedAutoSync({2,4,0,0,0,0,0,0}).valid);
}
TEST(AutoExperiment, AsyncOptionalTokenAndMalformedFailClosed) {
  auto r=ParseObservedAutoIndication({1,2,0,0,0});EXPECT_TRUE(r.valid);EXPECT_FALSE(r.token);
  r=ParseObservedAutoIndication({1,2,0,41,0,0x10,4,0,7,0,0,0});
  EXPECT_TRUE(r.valid);EXPECT_EQ(r.error,41);ASSERT_TRUE(r.token);EXPECT_EQ(*r.token,7u);
  for(auto b:std::vector<std::vector<uint8_t>>{{},{1,2,0,0},{1,2,0,0,0,1,2,0,0,0},
      {0x10,4,0,1,0,0,0},{1,2,0,0,0,0x11,1,0,0}})
    EXPECT_FALSE(ParseObservedAutoIndication(b).valid);
}
TEST(AutoExperiment, ExactCommandStillClosedAndNoGenericFeatureRoute) {
  EXPECT_TRUE(IsClosedAutoExperimentCommand({"enable-auto-selection-once","--confirm",kAutoExperimentConfirmation}));
  EXPECT_FALSE(IsClosedAutoExperimentCommand({"enable-auto-selection-once","--confirm","wrong"}));
  EXPECT_FALSE(IsClosedAutoExperimentCommand({"enable-auto-selection-once","--feature","0x11"}));
  EXPECT_FALSE(kAutoExperimentSendOpen);
}

TEST(AutoFeatureTyped, EnableDisableOnlyStockFieldsNoTokenCarrierOrRefresh) {
  EXPECT_EQ(BuildAutoFeatureRequest(FeatureSlot::kSlot0, true),
      (std::vector<uint8_t>{1,4,0,0,0,0,0,0x11,4,0,3,0,0,0,0x13,1,0,1}));
  EXPECT_EQ(BuildAutoFeatureRequest(FeatureSlot::kSlot1, false),
      (std::vector<uint8_t>{1,4,0,1,0,0,0,0x11,4,0,0,0,0,0,0x13,1,0,0}));
  EXPECT_TRUE(BuildAutoFeatureRequest(static_cast<FeatureSlot>(2), true).empty());
  EXPECT_FALSE(kAutoExperimentSendOpen);
}
TEST(AutoFeatureTyped, FeatureStateDoesNotClaimOriginalEfsRestoration) {
  EXPECT_EQ(ClassifyAutoFeature(3, true), AutoFeatureState::kEnabled);
  EXPECT_EQ(ClassifyAutoFeature(0, false), AutoFeatureState::kDisabled);
  EXPECT_EQ(ClassifyAutoFeature(3, false), AutoFeatureState::kUnknown);
  EXPECT_EQ(ClassifyAutoFeature(99, true), AutoFeatureState::kUnknown);
  EXPECT_EQ(ClassifyAutoFeature({}, true), AutoFeatureState::kUnknown);
  EXPECT_EQ(ClassifyAutoFeature(3, {}), AutoFeatureState::kUnknown);
  EXPECT_STREQ(kAutoOriginalRestoreEffect, "AUTO_FEATURE_PERSISTENT_PRE_SKT_EFS_NOT_RESTORED");
}
