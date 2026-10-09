#include "mapping_experiment.h"
#include "mapping_transport.h"
#include <gtest/gtest.h>
using namespace guacamole_pdc;
namespace {
GetSelectedConfigOutcome Selected(MappingTarget active = MappingTarget::kCommon,
    std::optional<MappingTarget> pending = {}) {
  GetSelectedConfigOutcome s; s.status = ProbeStatus::kOk; s.data.ok = true;
  s.data.active_id = MappingId(active);
  if (pending) s.data.pending_id = MappingId(*pending);
  return s;
}
GetConfigInfoOutcome Info(MappingTarget t) {
  GetConfigInfoOutcome o; o.status = ProbeStatus::kOk; o.data.ok = true;
  o.data.has_total_size = o.data.has_description = o.data.has_version = true;
  const bool common = t == MappingTarget::kCommon;
  o.data.total_size = common ? 24508 : 41172;
  o.data.description = common ? "Common-Commercial" : "Commercial-SKT";
  o.data.version = common ? 134285321 : 134293258;
  return o;
}
class MappingFake : public IMappingClient {
 public:
  GetSelectedConfigOutcome before = ::Selected();
  GetSelectedConfigOutcome after = ::Selected(MappingTarget::kCommon, MappingTarget::kSkt);
  GetConfigInfoOutcome common = ::Info(MappingTarget::kCommon);
  GetConfigInfoOutcome skt = ::Info(MappingTarget::kSkt);
  SetSelectedConfigOutcome result;
  int selected_calls = 0, info_calls = 0;
  std::vector<MappingTarget> selections;
  MappingFake() { result.status = ProbeStatus::kOk; result.data.ok = true; }
  GetSelectedConfigOutcome Selected() override { return ++selected_calls == 1 ? before : after; }
  GetConfigInfoOutcome Info(MappingTarget t) override {
    ++info_calls; return t == MappingTarget::kCommon ? common : skt;
  }
  SetSelectedConfigOutcome Select(MappingTarget t) override { selections.push_back(t); return result; }
};
MappingRequest Skt() { return {MappingCommand::kSelectSkt, kSktConfirmation}; }
MappingRequest Common() { return {MappingCommand::kSelectCommon, kCommonConfirmation}; }
}
TEST(MappingExperiment, PreflightCommonIdleAllowedNoSelection) {
  MappingFake c; const auto r = RunMappingExperiment(c, {});
  EXPECT_EQ(r.outcome, "PREFLIGHT_ALLOWED"); EXPECT_TRUE(c.selections.empty());
  EXPECT_EQ(c.selected_calls, 1); EXPECT_EQ(c.info_calls, 2);
}
TEST(MappingExperiment, WrongActiveBlockedBeforeSelection) {
  MappingFake c; c.before = Selected(MappingTarget::kSkt);
  EXPECT_EQ(RunMappingExperiment(c, Skt()).outcome, "EXPERIMENT_BLOCKED");
  EXPECT_TRUE(c.selections.empty());
}
TEST(MappingExperiment, PendingAlreadyPresentBlocked) {
  MappingFake c; c.before = Selected(MappingTarget::kCommon, MappingTarget::kSkt);
  EXPECT_EQ(RunMappingExperiment(c, Skt()).outcome, "EXPERIMENT_BLOCKED");
  EXPECT_TRUE(c.selections.empty());
}
TEST(MappingExperiment, MetadataMismatchOrMissingFieldBlocksEitherProfile) {
  for (bool common : {false, true}) for (int field = 0; field < 6; ++field) {
    MappingFake c; auto& d = common ? c.common.data : c.skt.data;
    if (field == 0) ++d.total_size;
    if (field == 1) d.description += "x";
    if (field == 2) ++d.version;
    if (field == 3) d.has_total_size = false;
    if (field == 4) d.has_description = false;
    if (field == 5) d.has_version = false;
    EXPECT_EQ(RunMappingExperiment(c, Skt()).outcome, "EXPERIMENT_BLOCKED");
    EXPECT_TRUE(c.selections.empty());
  }
}
TEST(MappingExperiment, GetInfoOrSelectedErrorBlocks) {
  for (int field = 0; field < 3; ++field) {
    MappingFake c;
    if (field == 0) c.before.status = ProbeStatus::kTimeout;
    if (field == 1) c.common.status = ProbeStatus::kIndicationRejected;
    if (field == 2) c.skt.status = ProbeStatus::kMalformedResponse;
    EXPECT_EQ(RunMappingExperiment(c, Skt()).outcome, "EXPERIMENT_BLOCKED");
    EXPECT_TRUE(c.selections.empty());
  }
}
TEST(MappingExperiment, WrongConfirmationMakesZeroCalls) {
  for (auto request : {MappingRequest{MappingCommand::kSelectSkt, kCommonConfirmation},
                      MappingRequest{MappingCommand::kSelectCommon, kSktConfirmation}}) {
    MappingFake c; RunMappingExperiment(c, request);
    EXPECT_EQ(c.selected_calls, 0); EXPECT_EQ(c.info_calls, 0); EXPECT_TRUE(c.selections.empty());
  }
}
TEST(MappingExperiment, OnlyExactCommandsNoArbitraryIdRoute) {
  EXPECT_TRUE(ParseMappingArguments({"preflight"}));
  EXPECT_TRUE(ParseMappingArguments({"select-skt-once", "--confirm", kSktConfirmation}));
  EXPECT_TRUE(ParseMappingArguments({"select-common-once", "--confirm", kCommonConfirmation}));
  EXPECT_FALSE(ParseMappingArguments({"select", "deadbeef"}));
  EXPECT_FALSE(ParseMappingArguments({"select-skt-once", "--confirm", kSktConfirmation, "deadbeef"}));
  EXPECT_FALSE(ParseMappingArguments({"preflight", "--id", "deadbeef"}));
  EXPECT_FALSE(ParseMappingArguments({"select-skt-once"}));
  EXPECT_FALSE(ParseMappingArguments({"select-common-once", "--confirm", kSktConfirmation}));
}
TEST(MappingExperiment, SktSendsOnceAndObservesPostOnce) {
  MappingFake c; const auto r = RunMappingExperiment(c, Skt());
  EXPECT_EQ(c.selections, (std::vector<MappingTarget>{MappingTarget::kSkt}));
  EXPECT_EQ(c.selected_calls, 2); EXPECT_TRUE(r.post_observed);
  EXPECT_EQ(r.outcome, "TARGET_OBSERVED_REVIEW_REQUIRED");
}
TEST(MappingExperiment, TimeoutNoRetryNoCommonRecoveryPostReadOnly) {
  MappingFake c; c.result.status = ProbeStatus::kTimeout;
  const auto r = RunMappingExperiment(c, Skt());
  EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED");
  EXPECT_EQ(r.selection.status, ProbeStatus::kTimeout);
  EXPECT_EQ(c.selections, (std::vector<MappingTarget>{MappingTarget::kSkt}));
  EXPECT_EQ(c.selected_calls, 2);
}
TEST(MappingExperiment, AllFailureStatusesPreservedWithOnePostObservation) {
  for (auto status : {ProbeStatus::kRequestSendFailed, ProbeStatus::kMalformedResponse,
       ProbeStatus::kIndicationRejected, ProbeStatus::kRequestRejected, ProbeStatus::kInitFailed}) {
    MappingFake c; c.result.status = status; c.result.data.error_status = 41;
    const auto r = RunMappingExperiment(c, Skt());
    EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED"); EXPECT_EQ(r.selection.status, status);
    EXPECT_EQ(r.selection.data.error_status, 41); EXPECT_EQ(c.selections.size(), 1u);
    EXPECT_EQ(c.selected_calls, 2);
  }
}
TEST(MappingExperiment, SuccessUnexpectedPostStopsWithoutRecovery) {
  MappingFake c; c.after = Selected();
  const auto r = RunMappingExperiment(c, Skt());
  EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED"); EXPECT_EQ(c.selections.size(), 1u);
}
TEST(MappingExperiment, SuccessButPostReadFailsIsUnknown) {
  MappingFake c; c.after.status = ProbeStatus::kTimeout;
  EXPECT_EQ(RunMappingExperiment(c, Skt()).outcome, "USER_REVIEW_REQUIRED");
  EXPECT_EQ(c.selected_calls, 2); EXPECT_EQ(c.selections.size(), 1u);
}
TEST(MappingExperiment, CommonIdleRecoveryNoop) {
  MappingFake c; EXPECT_EQ(RunMappingExperiment(c, Common()).outcome, "NOOP_OK");
  EXPECT_TRUE(c.selections.empty()); EXPECT_EQ(c.selected_calls, 1); EXPECT_EQ(c.info_calls, 2);
}
TEST(MappingExperiment, RecoveryRejectsUnknownOrAbsentActiveAndUnknownPending) {
  for (int which = 0; which < 3; ++which) {
    MappingFake c;
    if (which == 0) c.before.data.active_id = std::vector<uint8_t>{9};
    if (which == 1) c.before.data.active_id.reset();
    if (which == 2) c.before.data.pending_id = std::vector<uint8_t>{9};
    EXPECT_EQ(RunMappingExperiment(c, Common()).outcome, "RECOVERY_BLOCKED");
    EXPECT_TRUE(c.selections.empty());
  }
}
TEST(MappingExperiment, CommonRecoverySendsOnceAndObservesOnly) {
  MappingFake c; c.before = Selected(MappingTarget::kSkt); c.after = Selected();
  const auto r = RunMappingExperiment(c, Common());
  EXPECT_EQ(c.selections, (std::vector<MappingTarget>{MappingTarget::kCommon}));
  EXPECT_EQ(c.selected_calls, 2); EXPECT_EQ(r.outcome, "TARGET_OBSERVED_REVIEW_REQUIRED");
}
TEST(MappingExperiment, RecoveryTimeoutNeverRetries) {
  MappingFake c; c.before = Selected(MappingTarget::kCommon, MappingTarget::kSkt);
  c.result.status = ProbeStatus::kTimeout;
  EXPECT_EQ(RunMappingExperiment(c, Common()).outcome, "USER_REVIEW_REQUIRED");
  EXPECT_EQ(c.selections, (std::vector<MappingTarget>{MappingTarget::kCommon}));
  EXPECT_EQ(c.selected_calls, 2);
}
TEST(MappingExperiment, RecoveryMetadataMismatchBlocksEvenNoopCandidate) {
  MappingFake c; ++c.skt.data.version;
  EXPECT_EQ(RunMappingExperiment(c, Common()).outcome, "RECOVERY_BLOCKED");
  EXPECT_TRUE(c.selections.empty());
}
namespace {
class GuardTransport : public IQcciShim {
 public:
  int sends = 0;
  bool Load(std::string*) override { return true; }
  QmiServiceObject PdcGetServiceObject(int32_t, int32_t, int32_t) override { return this; }
  int ClientInitInstance(QmiServiceObject, QmiIndicationCallback, void*, uint32_t, QmiClientHandle*) override { return 0; }
  int ClientRelease(QmiClientHandle) override { return 0; }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int,
      void*, unsigned int, unsigned int*, unsigned int) override { ++sends; return -1; }
};
int Send(MappingQcciGuard& guard, unsigned int msg, const std::vector<uint8_t>& b) {
  uint8_t response[64]; unsigned int len = 0;
  return guard.SendRawMsgSync(nullptr, msg, b.data(), b.size(), response, sizeof(response), &len, 1);
}
}
TEST(MappingGuard, NoLoadActivateDeleteOrListCanReachTransport) {
  GuardTransport inner; MappingQcciGuard guard(inner, MappingTarget::kSkt);
  auto b = BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, MappingId(MappingTarget::kSkt));
  for (unsigned int msg : {0x24u, 0x25u, 0x26u, 0x27u, 0x28ffu}) Send(guard, msg, b);
  EXPECT_EQ(inner.sends, 0);
}
TEST(MappingGuard, SelectionPermitConsumedEvenOnSendFailure) {
  GuardTransport inner; MappingQcciGuard guard(inner, MappingTarget::kSkt);
  const auto b = BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, MappingId(MappingTarget::kSkt));
  Send(guard, kMsgSetSelectedConfig, b); Send(guard, kMsgSetSelectedConfig, b);
  EXPECT_EQ(inner.sends, 1);
}
TEST(MappingGuard, PreflightPermitNeverSelects) {
  GuardTransport inner; MappingQcciGuard guard(inner, std::nullopt);
  Send(guard, kMsgSetSelectedConfig, BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, MappingId(MappingTarget::kSkt)));
  EXPECT_EQ(inner.sends, 0);
}
TEST(MappingGuard, ArbitraryOrOtherTargetAndWrongConfigTypeBlocked) {
  GuardTransport inner; MappingQcciGuard guard(inner, MappingTarget::kSkt);
  for (const auto& id : {std::vector<uint8_t>{1,2}, MappingId(MappingTarget::kCommon)})
    Send(guard, kMsgSetSelectedConfig, BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, id));
  Send(guard, kMsgSetSelectedConfig, BuildSetSelectedConfigRequest(1, ConfigType::kPlatform, MappingId(MappingTarget::kSkt)));
  Send(guard, kMsgGetConfigInfo, BuildGetConfigInfoRequest(1, ConfigType::kSoftware, {1,2}));
  EXPECT_EQ(inner.sends, 0);
}
TEST(MappingGuard, ExactReadOnlyRequestsAllowed) {
  GuardTransport inner; MappingQcciGuard guard(inner, std::nullopt);
  Send(guard, kMsgGetSelectedConfig, BuildGetSelectedConfigRequest(1, ConfigType::kSoftware));
  for (auto target : {MappingTarget::kCommon, MappingTarget::kSkt})
    Send(guard, kMsgGetConfigInfo, BuildGetConfigInfoRequest(2, ConfigType::kSoftware, MappingId(target)));
  EXPECT_EQ(inner.sends, 3);
}
