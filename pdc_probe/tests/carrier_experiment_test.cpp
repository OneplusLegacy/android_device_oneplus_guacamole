#include "carrier_transport.h"
#include "presence_diagnostics.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
using namespace guacamole_pdc;
namespace {
GetConfigInfoOutcome Present(CarrierTarget t) {
  GetConfigInfoOutcome o; o.status = ProbeStatus::kOk; o.data.ok = true;
  o.data.has_total_size = o.data.has_description = o.data.has_version = true;
  if (t == CarrierTarget::kCommon) { o.data.total_size = 24508; o.data.description = "Common-Commercial"; o.data.version = 134285321; }
  else if (t == CarrierTarget::kSkt) { o.data.total_size = 41172; o.data.description = "Commercial-SKT"; o.data.version = 134293258; }
  else { o.data.total_size = CarrierFileProfile(t)->expected_size; o.data.description = t==CarrierTarget::kKt?"Commercial-KT":"Commercial-LGU"; o.data.version = t==CarrierTarget::kKt?134293504:134293000; }
  return o;
}
GetConfigInfoOutcome Rejected(uint16_t code) {
  GetConfigInfoOutcome o; o.status = ProbeStatus::kIndicationRejected;
  o.data.ok = true; o.data.error_status = code; return o;
}
CarrierRequest Request(CarrierOperation op, CarrierTarget target) {
  CarrierRequest r{op, target, ""}; r.confirmation = CarrierConfirmation(r); return r;
}
CarrierFile FakeFile(CarrierTarget target) {
  // Synthetic trusted file-reader boundary ONLY. Real guard rechecks hashes and
  // cannot be armed with these bytes. Never embed stock MBN payload in tests.
  CarrierFile f; f.verified = true; f.bytes.resize(CarrierFileProfile(target)->expected_size, 0x42); return f;
}
class FakeCarrier : public ICarrierClient {
 public:
  int reads = 0, selects = 0;
  std::vector<size_t> offsets;
  std::array<int, 4> info_calls{};
  std::array<GetConfigInfoOutcome, 4> infos{Present(CarrierTarget::kCommon), Present(CarrierTarget::kSkt), Present(CarrierTarget::kKt), Present(CarrierTarget::kLgu)};
  GetSelectedConfigOutcome before, after;
  ProbeStatus chunk_status = ProbeStatus::kOk, select_status = ProbeStatus::kOk;
  bool bad_remaining = false, post_present = false;
  FakeCarrier() {
    before.status = ProbeStatus::kOk; before.data.ok = true;
    before.data.active_id = CarrierId(CarrierTarget::kCommon); after = before;
  }
  GetSelectedConfigOutcome Selected() override { return ++reads == 1 ? before : after; }
  GetConfigInfoOutcome Info(CarrierTarget t) override {
    const auto n = ++info_calls[static_cast<size_t>(t)];
    return post_present && n > 1 ? Present(t) : infos[static_cast<size_t>(t)];
  }
  LoadConfigChunkOutcome Chunk(CarrierTarget t, size_t offset) override {
    offsets.push_back(offset);
    LoadConfigChunkOutcome o; o.status = chunk_status; o.data.ok = chunk_status == ProbeStatus::kOk;
    const size_t total = CarrierFileProfile(t)->expected_size;
    const size_t len = std::min(kLoadChunkSize, total - offset);
    o.data.has_received = o.data.has_remaining_size = true;
    o.data.received = len; o.data.remaining_size = total - offset - len + (bad_remaining ? 1 : 0);
    return o;
  }
  SetSelectedConfigOutcome Select(CarrierTarget) override {
    ++selects; SetSelectedConfigOutcome o; o.status = select_status;
    o.data.ok = select_status == ProbeStatus::kOk; return o;
  }
};
class WireSink : public IQcciShim {
 public:
  int sends = 0;
  bool Load(std::string*) override { return true; }
  QmiServiceObject PdcGetServiceObject(int32_t, int32_t, int32_t) override { return this; }
  int ClientInitInstance(QmiServiceObject, QmiIndicationCallback, void*, uint32_t, QmiClientHandle*) override { return 0; }
  int ClientRelease(QmiClientHandle) override { return 0; }
  int SendRawMsgSync(QmiClientHandle, unsigned int, const void*, unsigned int, void*, unsigned int, unsigned int*, unsigned int) override { ++sends; return -1; }
};
void Send(CarrierQcciGuard& g, unsigned int msg, const std::vector<uint8_t>& b) {
  uint8_t response[64]; unsigned int received = 0;
  g.SendRawMsgSync(nullptr, msg, b.data(), b.size(), response, sizeof(response), &received, 1);
}
}
TEST(CarrierIdentity, ExactIdsAndTwentyByteWireId) {
  const std::vector<uint8_t> kt{0x79,0xbd,0x2e,0x28,0xf6,0x13,0x10,0xfd,0x53,0x8c,0x0a,0x5b,0x9b,0xcb,0xd1,0x9f,0x14,0x3d,0xa7,0x62};
  const std::vector<uint8_t> lgu{0x3a,0x87,0xa2,0xce,0x83,0x08,0x26,0xe7,0xde,0x1f,0x1a,0x8c,0xb2,0xfa,0xf5,0x01,0x2b,0x4b,0x71,0xbc};
  EXPECT_EQ(CarrierId(CarrierTarget::kKt), kt); EXPECT_EQ(CarrierId(CarrierTarget::kLgu), lgu);
  for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu}) {
    const auto wire = BuildGetConfigInfoRequest(7, ConfigType::kSoftware, CarrierId(t));
    ASSERT_EQ(wire.size(), 35u);
    EXPECT_EQ(std::vector<uint8_t>(wire.begin(), wire.begin() + 8), (std::vector<uint8_t>{1,25,0,1,0,0,0,20}));
    EXPECT_EQ(std::vector<uint8_t>(wire.begin() + 8, wire.begin() + 28), CarrierId(t));
  }
}
TEST(CarrierIdentity, OnlyValidIndicationNoEntryMeansAbsent) {
  EXPECT_EQ(CarrierPresence(Rejected(50)), ConfigPresence::kAbsent);
  EXPECT_EQ(CarrierPresence(Rejected(41)), ConfigPresence::kUnknown);
  auto invalid = Rejected(50); invalid.data.ok = false;
  EXPECT_EQ(CarrierPresence(invalid), ConfigPresence::kUnknown);
  invalid = Rejected(1); invalid.status = ProbeStatus::kRequestRejected; invalid.data.error_code = 50;
  EXPECT_EQ(CarrierPresence(invalid), ConfigPresence::kUnknown); // preserve stricter existing sync policy
}
TEST(CarrierIdentity, ArbitraryIdPathAndUnsupportedCommandsHaveNoCliRoute) {
  for (const auto& a : std::vector<std::vector<std::string>>{{"info", "deadbeef"}, {"load-kt-once", "/tmp/evil"},
      {"activate"}, {"load-skt-once", "--confirm", "x"}, {"select-skt-once", "--confirm", kSktConfirmation},
      {"inspect-kt", "extra"}, {"delete"}}) EXPECT_FALSE(ParseCarrierArguments(a));
  EXPECT_TRUE(ParseCarrierArguments({"inspect-kt"})); EXPECT_TRUE(ParseCarrierArguments({"inspect-lgu"}));
}
TEST(CarrierIdentity, InspectAllIsReadOnlyAndPreservesUnknown) {
  FakeCarrier c; c.infos[2] = c.infos[3] = Rejected(41);
  const auto r = RunCarrierExperiment(c, {}, {});
  EXPECT_EQ(r.outcome, "READ_ONLY_OBSERVED"); EXPECT_EQ(r.info[2].data.error_status, 41);
  EXPECT_EQ(c.selects, 0); EXPECT_TRUE(c.offsets.empty()); EXPECT_EQ(c.reads, 0);
}
TEST(CarrierLoad, WrongConfirmationNoCalls) {
  FakeCarrier c; auto req = Request(CarrierOperation::kLoad, CarrierTarget::kKt); req.confirmation = "wrong";
  RunCarrierExperiment(c, req, FakeFile(req.target));
  EXPECT_EQ(c.reads, 0); EXPECT_TRUE(c.offsets.empty()); EXPECT_EQ(c.selects, 0);
}
TEST(CarrierLoad, WrongHashOrFileVerificationNeverLoads) {
  for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu}) {
    FakeCarrier c; c.infos[static_cast<size_t>(t)] = Rejected(50);
    auto file = FakeFile(t); EXPECT_FALSE(VerifyCarrierBytes(t, file.bytes));
    file.verified = false;
    EXPECT_EQ(RunCarrierExperiment(c, Request(CarrierOperation::kLoad, t), file).outcome, "LOAD_BLOCKED_FILE");
    EXPECT_TRUE(c.offsets.empty());
  }
}
TEST(CarrierLoad, ExistingExactInfoNoop) {
  FakeCarrier c;
  EXPECT_EQ(RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), {}).outcome, "ALREADY_PRESENT_NOOP");
  EXPECT_TRUE(c.offsets.empty()); EXPECT_EQ(c.selects, 0);
}
TEST(CarrierLoad, Error41BlocksNotAbsent) {
  FakeCarrier c; c.infos[2] = Rejected(41);
  EXPECT_EQ(RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), FakeFile(CarrierTarget::kKt)).outcome,
            "LOAD_BLOCKED_UNKNOWN_PRESTATE");
  EXPECT_TRUE(c.offsets.empty());
}
TEST(CarrierLoad, OneLogicalLoadEachChunkOnceThenPostInfoAndSelectedNoSelection) {
  FakeCarrier c; c.infos[2] = Rejected(50);
  const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), FakeFile(CarrierTarget::kKt));
  EXPECT_TRUE(r.mutation_attempted); EXPECT_TRUE(r.post_info_observed); EXPECT_TRUE(r.post_observed);
  EXPECT_EQ(c.offsets.size(), (39544u + 1023u) / 1024u);
  EXPECT_EQ(std::set<size_t>(c.offsets.begin(), c.offsets.end()).size(), c.offsets.size());
  EXPECT_EQ(c.info_calls[2], 2); EXPECT_EQ(c.reads, 2); EXPECT_EQ(c.selects, 0);
  EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED"); // post absence never becomes success
}
TEST(CarrierLoad, TimeoutMalformedAndSendFailureNeverRetryOrRecover) {
  for (auto status : {ProbeStatus::kTimeout, ProbeStatus::kMalformedResponse, ProbeStatus::kRequestSendFailed}) {
    FakeCarrier c; c.infos[3] = Rejected(50); c.chunk_status = status;
    const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kLgu), FakeFile(CarrierTarget::kLgu));
    EXPECT_EQ(c.offsets.size(), 1u); EXPECT_EQ(c.selects, 0); EXPECT_EQ(c.reads, 2);
    EXPECT_EQ(r.last_chunk.status, status); EXPECT_TRUE(r.post_info_observed);
  }
}
TEST(CarrierLoad, BadRemainingStopsBeforeNextChunk) {
  FakeCarrier c; c.infos[2] = Rejected(50); c.bad_remaining = true;
  RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), FakeFile(CarrierTarget::kKt));
  EXPECT_EQ(c.offsets.size(), 1u);
}
TEST(CarrierSelect, AbsentUnknownMissingMetadataBlocked) {
  for (auto o : {Rejected(50), Rejected(41), GetConfigInfoOutcome{}}) {
    FakeCarrier c; c.infos[2] = o;
    EXPECT_FALSE(RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kKt), {}).mutation_attempted);
    EXPECT_EQ(c.selects, 0);
  }
  FakeCarrier c; c.infos[2].data.has_version = false;
  EXPECT_FALSE(RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kKt), {}).mutation_attempted);
}
TEST(CarrierSelect, ExactPresentSendsOncePostExactlyOnceEvenOnTimeout) {
  for (auto s : {ProbeStatus::kOk, ProbeStatus::kTimeout, ProbeStatus::kMalformedResponse}) {
    FakeCarrier c; c.select_status = s;
    const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kLgu), {});
    EXPECT_TRUE(r.mutation_attempted); EXPECT_EQ(c.selects, 1); EXPECT_EQ(c.reads, 2);
    EXPECT_TRUE(c.offsets.empty()); EXPECT_EQ(r.selection.status, s); EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED");
  }
}
TEST(CarrierSelect, UnexpectedPostNeverAutoRecovers) {
  FakeCarrier c; c.after.data.active_id = std::vector<uint8_t>{99};
  const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kKt), {});
  EXPECT_EQ(c.selects, 1); EXPECT_EQ(c.reads, 2); EXPECT_EQ(r.post.data.active_id, c.after.data.active_id);
}
TEST(CarrierRecovery, CommonIdleNoopAndUnknownIdBlocks) {
  FakeCarrier c;
  EXPECT_EQ(RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kCommon), {}).outcome, "NOOP_OK");
  EXPECT_EQ(c.selects, 0);
  FakeCarrier bad; bad.before.data.pending_id = std::vector<uint8_t>{99};
  EXPECT_FALSE(RunCarrierExperiment(bad, Request(CarrierOperation::kSelect, CarrierTarget::kCommon), {}).mutation_attempted);
}
TEST(CarrierRecovery, KtLguRequireRuntimeInfoSuccessNotJustCompileTimeMembership) {
  for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu}) {
    FakeCarrier c; c.before.data.pending_id = CarrierId(t); c.infos[static_cast<size_t>(t)] = Rejected(41);
    EXPECT_FALSE(RunCarrierExperiment(c, Request(CarrierOperation::kSelect, CarrierTarget::kCommon), {}).mutation_attempted);
    EXPECT_EQ(c.selects, 0);
    FakeCarrier good; good.before.data.pending_id = CarrierId(t); good.select_status = ProbeStatus::kTimeout;
    EXPECT_TRUE(RunCarrierExperiment(good, Request(CarrierOperation::kSelect, CarrierTarget::kCommon), {}).mutation_attempted);
    EXPECT_EQ(good.selects, 1); EXPECT_EQ(good.reads, 2);
  }
}
TEST(CarrierGuard, ReadOnlyExactIdsAndNoUnknownPath) {
  WireSink sink; CarrierQcciGuard g(sink, {});
  for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu})
    Send(g, kMsgGetConfigInfo, BuildGetConfigInfoRequest(1, ConfigType::kSoftware, CarrierId(t)));
  EXPECT_EQ(sink.sends, 2);
  Send(g, kMsgGetConfigInfo, BuildGetConfigInfoRequest(1, ConfigType::kSoftware, {1,2}));
  EXPECT_EQ(sink.sends, 2);
}
TEST(CarrierGuard, NoActivateDeleteLoadOrSelectWithoutExplicitPermit) {
  WireSink sink; CarrierQcciGuard g(sink, {});
  const auto b = BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, CarrierId(CarrierTarget::kKt));
  for (unsigned int msg : {0x23u,0x24u,0x25u,0x26u,0x27u,0x2bu}) Send(g, msg, b);
  EXPECT_EQ(sink.sends, 0);
}
TEST(CarrierGuard, ArmedSelectOnlyExactTargetOnceEvenAfterSendFailure) {
  WireSink sink; CarrierQcciGuard g(sink, Request(CarrierOperation::kSelect, CarrierTarget::kKt));
  ASSERT_TRUE(g.Arm({}));
  Send(g, kMsgSetSelectedConfig, BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, CarrierId(CarrierTarget::kLgu)));
  EXPECT_EQ(sink.sends, 0);
  const auto b = BuildSetSelectedConfigRequest(2, ConfigType::kSoftware, CarrierId(CarrierTarget::kKt));
  Send(g, kMsgSetSelectedConfig, b); Send(g, kMsgSetSelectedConfig, b);
  EXPECT_EQ(sink.sends, 1); EXPECT_FALSE(g.Arm({}));
}
TEST(CarrierGuard, WrongHashCannotArmRealLoadTransport) {
  WireSink sink; CarrierQcciGuard g(sink, Request(CarrierOperation::kLoad, CarrierTarget::kKt));
  const auto f = FakeFile(CarrierTarget::kKt);
  EXPECT_FALSE(g.Arm(f));
  Send(g, kMsgLoadConfig, BuildLoadConfigChunkRequest(1, ConfigType::kSoftware, CarrierId(CarrierTarget::kKt), f.bytes.size(), f.bytes.data(), 1024));
  EXPECT_EQ(sink.sends, 0);
}
TEST(CarrierGuard, ExactChunkWireComparatorRejectsWrongIdSizePayloadAndOffset) {
  const auto f = FakeFile(CarrierTarget::kKt);
  const auto b = BuildLoadConfigChunkRequest(1, ConfigType::kSoftware, CarrierId(CarrierTarget::kKt), f.bytes.size(), f.bytes.data(), 1024);
  EXPECT_TRUE(CarrierLoadRequestMatches(CarrierTarget::kKt, f.bytes, 0, 1, b));
  EXPECT_FALSE(CarrierLoadRequestMatches(CarrierTarget::kLgu, f.bytes, 0, 1, b));
  auto corrupt = b; corrupt[8] ^= 1;
  EXPECT_FALSE(CarrierLoadRequestMatches(CarrierTarget::kKt, f.bytes, 0, 1, corrupt));
  for (size_t index : {size_t(28), size_t(34)}) {
    auto bad = b; bad[index] ^= 1; // full size or actual chunk payload
    EXPECT_FALSE(CarrierLoadRequestMatches(CarrierTarget::kKt, f.bytes, 0, 1, bad));
  }
  EXPECT_FALSE(CarrierLoadRequestMatches(CarrierTarget::kKt, f.bytes, f.bytes.size(), 1, b));
  EXPECT_FALSE(CarrierLoadRequestMatches(CarrierTarget::kKt, f.bytes, 0, 2, b));
}

TEST(CarrierLoad, PostSuccessStillRequiresReviewAndDoesNotSelect) {
  FakeCarrier c; c.infos[2] = Rejected(50); c.post_present = true;
  const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), FakeFile(CarrierTarget::kKt));
  EXPECT_TRUE(r.transfer_completed); EXPECT_TRUE(r.load_post_verified);
  EXPECT_EQ(r.outcome, "USER_REVIEW_REQUIRED"); EXPECT_EQ(c.selects, 0);
}
TEST(CarrierLoad, PostSelectedChangeCannotVerifyLoadInvariant) {
  FakeCarrier c; c.infos[2] = Rejected(50); c.post_present = true;
  c.after.data.pending_id = CarrierId(CarrierTarget::kKt);
  const auto r = RunCarrierExperiment(c, Request(CarrierOperation::kLoad, CarrierTarget::kKt), FakeFile(CarrierTarget::kKt));
  EXPECT_TRUE(r.transfer_completed); EXPECT_FALSE(r.load_post_verified);
  EXPECT_EQ(c.selects, 0);
}

namespace {
class H41Carrier : public FakeCarrier {
 public:
  int lists=0, synthetics=0;
  ListConfigsOutcome listing;
  GetConfigInfoOutcome synthetic=Rejected(41);
  H41Carrier() {
    infos[2]=infos[3]=Rejected(41);
    listing.status=ProbeStatus::kOk; listing.data.ok=true;
    listing.data.configs={{ConfigType::kSoftware,CarrierId(CarrierTarget::kCommon)},
        {ConfigType::kSoftware,CarrierId(CarrierTarget::kSkt)}};
  }
  ListConfigsOutcome List() override { ++lists; return listing; }
  GetConfigInfoOutcome SyntheticInfo() override { ++synthetics; return synthetic; }
};
CarrierReport H41Run(H41Carrier& c, const CarrierFile& f=FakeFile(CarrierTarget::kKt)) {
  return RunCarrierExperiment(c,Request(CarrierOperation::kH41Load,CarrierTarget::kKt),f);
}
}
TEST(H41Load, StrongConfirmationSeparateFromLegacy) {
  EXPECT_TRUE(ParseCarrierArguments({"load-kt-h41-once","--confirm","GUACAMOLE_KT_H41_LOAD_EXPERIMENT_V1"}));
  EXPECT_TRUE(ParseCarrierArguments({"load-lgu-h41-once","--confirm","GUACAMOLE_LGU_H41_LOAD_EXPERIMENT_V1"}));
  EXPECT_FALSE(ParseCarrierArguments({"load-kt-once","--confirm","GUACAMOLE_KT_H41_LOAD_EXPERIMENT_V1"}));
  EXPECT_FALSE(ParseCarrierArguments({"load-kt-h41-once","--confirm","GUACAMOLE_KT_LOAD_EXPERIMENT_V1"}));
  H41Carrier c; auto req=Request(CarrierOperation::kH41Load,CarrierTarget::kKt);req.confirmation="wrong";
  RunCarrierExperiment(c,req,FakeFile(req.target));
  EXPECT_EQ(c.reads,0); EXPECT_EQ(c.lists,0); EXPECT_TRUE(c.offsets.empty());
}
TEST(H41Load, CandidateSameInvocationAnd39ChunksPostReadOnly) {
  H41Carrier c; c.post_present=true; const auto r=H41Run(c);
  EXPECT_EQ(r.load_gate,"UNREGISTERED_CANDIDATE_H41");
  EXPECT_EQ(CarrierPresence(r.info[2]),ConfigPresence::kUnknown);
  EXPECT_EQ(c.lists,1);EXPECT_EQ(c.synthetics,1);EXPECT_EQ(c.info_calls[0],1);EXPECT_EQ(c.info_calls[1],1);
  EXPECT_EQ(c.info_calls[2],2);EXPECT_EQ(c.reads,2);EXPECT_EQ(c.selects,0);
  ASSERT_EQ(c.offsets.size(),39u);
  for(size_t i=0;i<c.offsets.size();++i) EXPECT_EQ(c.offsets[i],i*1024u);
  EXPECT_TRUE(r.transfer_completed);EXPECT_TRUE(r.load_post_verified);
  EXPECT_EQ(r.outcome,"USER_REVIEW_REQUIRED");
}
TEST(H41Load, SyntheticMustBeDecodedAsync41) {
  for(auto o:{Rejected(50),GetConfigInfoOutcome{},Present(CarrierTarget::kKt)}) {
    H41Carrier c;c.synthetic=o;EXPECT_FALSE(H41Run(c).mutation_attempted);
  }
  H41Carrier c;c.synthetic.data.ok=false;EXPECT_FALSE(H41Run(c).mutation_attempted);
  H41Carrier d;d.synthetic.status=ProbeStatus::kRequestRejected;EXPECT_FALSE(H41Run(d).mutation_attempted);
}
TEST(H41Load, BothListOmissionsRequiredAndListMustBeValidComplete) {
  for(auto id:{CarrierId(CarrierTarget::kKt),PresenceDiagnosticId()}) {
    H41Carrier c;c.listing.data.configs.push_back({ConfigType::kSoftware,id});
    EXPECT_FALSE(H41Run(c).mutation_attempted);EXPECT_TRUE(c.offsets.empty());
  }
  H41Carrier c;c.listing.data.truncated=true;EXPECT_FALSE(H41Run(c).mutation_attempted);
  H41Carrier d;d.listing.status=ProbeStatus::kTimeout;EXPECT_FALSE(H41Run(d).mutation_attempted);
  H41Carrier e;e.listing.data.ok=false;EXPECT_FALSE(H41Run(e).mutation_attempted);
}
TEST(H41Load, CommonIdleAndKnownMetadataMandatory) {
  H41Carrier c;c.before.data.active_id=CarrierId(CarrierTarget::kSkt);EXPECT_FALSE(H41Run(c).mutation_attempted);
  H41Carrier d;d.before.data.pending_id=CarrierId(CarrierTarget::kKt);EXPECT_FALSE(H41Run(d).mutation_attempted);
  for(size_t i:{0u,1u}) {
    H41Carrier e;e.infos[i].data.version++;EXPECT_FALSE(H41Run(e).mutation_attempted);
  }
}
TEST(H41Load, WrongFileAndRealHashNeverArm) {
  H41Carrier c;auto f=FakeFile(CarrierTarget::kKt);f.verified=false;
  EXPECT_FALSE(H41Run(c,f).mutation_attempted);
  H41Carrier d;f.verified=true;f.bytes.pop_back();EXPECT_FALSE(H41Run(d,f).mutation_attempted);
  H41Carrier e;f=FakeFile(CarrierTarget::kKt);const auto report=H41Run(e,f);
  WireSink sink;CarrierQcciGuard g(sink,Request(CarrierOperation::kH41Load,CarrierTarget::kKt));
  EXPECT_FALSE(g.ArmH41(f,report)); // synthetic bytes cannot pass production hash boundary
  EXPECT_EQ(sink.sends,0);
}
TEST(H41Load, Async50UsesProtocolPathAndExistingProfileIsNoop) {
  H41Carrier c;c.infos[2]=Rejected(50);c.synthetic=Rejected(50);
  EXPECT_EQ(H41Run(c).load_gate,"ABSENT_PROTOCOL_CONFIRMED");
  H41Carrier d;d.infos[2]=Present(CarrierTarget::kKt);
  EXPECT_EQ(H41Run(d).outcome,"ALREADY_PRESENT_NOOP");EXPECT_TRUE(d.offsets.empty());
  H41Carrier e;e.infos[2]=Rejected(1);e.infos[2].status=ProbeStatus::kRequestRejected;e.infos[2].data.error_code=50;
  EXPECT_FALSE(H41Run(e).mutation_attempted);
}
TEST(H41Load, FailureNeverRetriesAndAlwaysPostObserves) {
  for(auto s:{ProbeStatus::kTimeout,ProbeStatus::kMalformedResponse,ProbeStatus::kRequestSendFailed}) {
    H41Carrier c;c.chunk_status=s;const auto r=H41Run(c);
    EXPECT_EQ(c.offsets.size(),1u);EXPECT_EQ(c.selects,0);EXPECT_EQ(c.info_calls[2],2);EXPECT_EQ(c.reads,2);
    EXPECT_TRUE(r.post_info_observed);EXPECT_TRUE(r.post_observed);EXPECT_FALSE(r.load_post_verified);
  }
}
TEST(H41Load, PostSelectedChangeNeverVerified) {
  H41Carrier c;c.post_present=true;c.after.data.pending_id=CarrierId(CarrierTarget::kKt);
  EXPECT_FALSE(H41Run(c).load_post_verified);EXPECT_EQ(c.selects,0);
}
TEST(H41Load, TransportReadAllowanceDoesNotGrantMutation) {
  WireSink sink;CarrierQcciGuard g(sink,Request(CarrierOperation::kH41Load,CarrierTarget::kKt));
  Send(g,kMsgListConfigs,BuildListConfigsRequest(1,ConfigType::kSoftware));
  Send(g,kMsgGetConfigInfo,BuildGetConfigInfoRequest(2,ConfigType::kSoftware,PresenceDiagnosticId()));
  EXPECT_EQ(sink.sends,2);
  const auto fake=FakeFile(CarrierTarget::kKt);
  EXPECT_FALSE(g.Arm(fake)); // ordinary permit cannot open H41 path
  Send(g,kMsgLoadConfig,BuildLoadConfigChunkRequest(3,ConfigType::kSoftware,CarrierId(CarrierTarget::kKt),fake.bytes.size(),fake.bytes.data(),1024));
  for(unsigned int msg:{0x23u,0x25u,0x27u,0x2bu,0x2eu})
    Send(g,msg,BuildGetSelectedConfigRequest(4,ConfigType::kSoftware));
  EXPECT_EQ(sink.sends,2);
}

TEST(H41Load, TargetMustBeWellFormedAsync41WithZeroSecondaryError) {
  H41Carrier c; c.infos[2].data.ok = false;
  EXPECT_FALSE(H41Run(c).mutation_attempted);
  H41Carrier d; d.infos[2].data.error_code = 1;
  EXPECT_FALSE(H41Run(d).mutation_attempted);
  H41Carrier e; e.synthetic.data.error_code = 1;
  EXPECT_FALSE(H41Run(e).mutation_attempted);
}
TEST(CarrierGuard, AutoMessageBlockedEvenWithArmedSelectionPermit) {
  WireSink sink;
  CarrierQcciGuard guard(sink, Request(CarrierOperation::kSelect, CarrierTarget::kKt));
  ASSERT_TRUE(guard.Arm({}));
  Send(guard, 0x2e, BuildGetSelectedConfigRequest(7, ConfigType::kSoftware));
  EXPECT_EQ(sink.sends, 0);
}
