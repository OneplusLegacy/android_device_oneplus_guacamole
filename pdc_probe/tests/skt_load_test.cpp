// Phase C0 state machine tests, against a fake IPdcClient - no real
// device, no real MBN bytes (synthetic buffers only, per the Phase C0
// spec's explicit requirement).
//
// Two real-device findings drive this file's design:
//
// 1. Indication field semantics (CONFIRMED, real-device test #1):
//    "Received" describes the CURRENT chunk's own size (optional/
//    advisory - the reference client never even reads it), and
//    "Remaining Size" is the sole authoritative progress field.
//
// 2. Config existence semantics (CONFIRMED, real-device test #2): a
//    real config (CZ-VoLTE-Vodafone) vanished from one List Configs
//    call and reappeared in a later one, while `Get Config Info` on its
//    exact ID succeeded throughout. List Configs is therefore
//    DIAGNOSTIC ONLY from here on - every existence/success decision in
//    this state machine is driven exclusively by Get Config Info on the
//    exact target config ID, classified via ClassifyConfigPresence()
//    (see pdc_client.h / pdc_wire.cpp - EXISTS / ABSENT / UNKNOWN,
//    fail-closed on UNKNOWN).

#include "skt_load.h"

#include <functional>

#include <gtest/gtest.h>

#include "hash.h"

using namespace guacamole_pdc;

namespace {

class FakePdcClient : public IPdcClient {
 public:
  std::function<ListConfigsOutcome(uint32_t)> on_list_configs;
  std::function<GetConfigInfoOutcome(ConfigType, const std::vector<uint8_t>&,
                                      uint32_t)>
      on_get_config_info;
  std::function<GetSelectedConfigOutcome(ConfigType, uint32_t)>
      on_get_selected_config;
  std::function<LoadConfigChunkOutcome(ConfigType, const std::vector<uint8_t>&,
                                        uint32_t, const uint8_t*, size_t,
                                        uint32_t)>
      on_load_config_chunk;
  // Set only if a future test needs it - RunLoadCarrier()/skt_load.cpp's
  // state machine never calls SetSelectedConfig() (see pdc_client.h's
  // own comment: host-test-only, not reachable from any CLI/orchestration
  // path yet), so no existing test configures this callback.
  std::function<SetSelectedConfigOutcome(ConfigType, const std::vector<uint8_t>&,
                                          uint32_t)>
      on_set_selected_config;

  int list_configs_calls = 0;
  int get_config_info_calls = 0;
  int get_selected_config_calls = 0;
  int load_config_chunk_calls = 0;
  int set_selected_config_calls = 0;
  std::vector<size_t> chunk_lens_seen;

  ListConfigsOutcome ListConfigs(uint32_t timeout_ms) override {
    list_configs_calls++;
    return on_list_configs(timeout_ms);
  }
  GetConfigInfoOutcome GetConfigInfo(ConfigType type,
                                      const std::vector<uint8_t>& id,
                                      uint32_t timeout_ms) override {
    get_config_info_calls++;
    return on_get_config_info(type, id, timeout_ms);
  }
  GetSelectedConfigOutcome GetSelectedConfig(ConfigType type,
                                              uint32_t timeout_ms) override {
    get_selected_config_calls++;
    return on_get_selected_config(type, timeout_ms);
  }
  LoadConfigChunkOutcome LoadConfigChunk(ConfigType type,
                                          const std::vector<uint8_t>& id,
                                          uint32_t total_size,
                                          const uint8_t* chunk_bytes,
                                          size_t chunk_len,
                                          uint32_t timeout_ms) override {
    load_config_chunk_calls++;
    chunk_lens_seen.push_back(chunk_len);
    return on_load_config_chunk(type, id, total_size, chunk_bytes, chunk_len,
                                 timeout_ms);
  }
  SetSelectedConfigOutcome SetSelectedConfig(ConfigType type,
                                              const std::vector<uint8_t>& id,
                                              uint32_t timeout_ms) override {
    set_selected_config_calls++;
    if (on_set_selected_config) {
      return on_set_selected_config(type, id, timeout_ms);
    }
    return SetSelectedConfigOutcome{};  // unused by any existing test.
  }
};

std::vector<uint8_t> MakeSyntheticFile(size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; i++) data[i] = static_cast<uint8_t>(i % 251);
  return data;
}

KnownProfile MakeProfile(const std::vector<uint8_t>& bytes) {
  KnownProfile p;
  p.name = "synthetic-test-profile";
  p.runtime_path = "/nonexistent/synthetic/path";  // never opened by
                                                     // RunLoadFromBytes.
  p.expected_size = static_cast<uint32_t>(bytes.size());
  p.expected_sha256 = Sha256(bytes.data(), bytes.size());
  return p;
}

std::vector<uint8_t> Sha1Vec(const std::vector<uint8_t>& bytes) {
  auto d = Sha1(bytes.data(), bytes.size());
  return std::vector<uint8_t>(d.begin(), d.end());
}

// --- Get Config Info canned outcomes, matching ClassifyConfigPresence's
// three real classification paths (see pdc_wire.cpp). ---

GetConfigInfoOutcome ExistsInfo(uint32_t total_size) {
  GetConfigInfoOutcome o;
  o.status = ProbeStatus::kOk;
  o.data.ok = true;
  o.data.has_total_size = true;
  o.data.total_size = total_size;
  return o;
}

// The specific, well-evidenced "no such id" answer -
// QMI_PROTOCOL_ERROR_NO_ENTRY (50) at the indication stage.
GetConfigInfoOutcome AbsentInfo() {
  GetConfigInfoOutcome o;
  o.status = ProbeStatus::kIndicationRejected;
  o.data.error_status = kQmiProtocolErrorNoEntry;
  return o;
}

// Some OTHER failure - must classify as kUnknown, NEVER kAbsent.
GetConfigInfoOutcome UnknownInfo() {
  GetConfigInfoOutcome o;
  o.status = ProbeStatus::kTimeout;
  return o;
}

// Default "nothing selected, nothing changes" stand-in for tests that
// don't care about the selection-safety check specifically. Both
// before/after calls return the identical (empty) snapshot, so
// selection_check ends up kUnchanged.
GetSelectedConfigOutcome NoneSelected() {
  GetSelectedConfigOutcome o;
  o.status = ProbeStatus::kOk;
  return o;
}

ListConfigsOutcome EmptyList() {
  ListConfigsOutcome o;
  o.status = ProbeStatus::kOk;
  o.data.ok = true;
  return o;
}

ListConfigsOutcome ListWithId(const std::vector<uint8_t>& id) {
  ListConfigsOutcome o;
  o.status = ProbeStatus::kOk;
  o.data.ok = true;
  ConfigEntry e;
  e.type = ConfigType::kSoftware;
  e.id = id;
  o.data.configs.push_back(e);
  return o;
}

// A "clean" chunk indication matching CONFIRMED real-device semantics:
// Received = THIS chunk's own size (not cumulative), Remaining Size =
// total - cumulative bytes sent so far (the authoritative field).
LoadConfigChunkOutcome CleanChunkOutcome(uint32_t current_chunk_len,
                                          uint32_t cumulative_after_this_chunk,
                                          uint32_t total) {
  LoadConfigChunkOutcome o;
  o.status = ProbeStatus::kOk;
  o.data.ok = true;
  o.data.error_status = 0;
  o.data.has_received = true;
  o.data.received = current_chunk_len;
  o.data.has_remaining_size = true;
  o.data.remaining_size = total - cumulative_after_this_chunk;
  o.data.has_frame_reset = true;
  o.data.frame_reset = false;
  return o;
}

// Convenience: a GetConfigInfo fixture that returns AbsentInfo() on the
// first call (DISCOVER) and ExistsInfo(total) on every call after
// (VERIFY_INFO / post-mortem) - the common "successful load" shape.
std::function<GetConfigInfoOutcome(ConfigType, const std::vector<uint8_t>&,
                                    uint32_t)>
AbsentThenExists(const FakePdcClient& fake, uint32_t total) {
  return [&fake, total](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return fake.get_config_info_calls == 1 ? AbsentInfo() : ExistsInfo(total);
  };
}

}  // namespace

TEST(SktLoad, SuccessfulLoadOneChunk) {
  auto file = MakeSyntheticFile(500);  // < kLoadChunkSize: exactly one chunk.
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };  // diagnostic only.
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  EXPECT_EQ(report.chunks_sent, 1u);
  EXPECT_EQ(report.bytes_sent, 500u);
  EXPECT_EQ(fake.load_config_chunk_calls, 1);
  EXPECT_EQ(report.config_presence_before, ConfigPresence::kAbsent);
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kExists);
  EXPECT_EQ(report.selection_check, SelectionCheckResult::kUnchanged);
  EXPECT_TRUE(report.selected_before.queried);
  EXPECT_TRUE(report.selected_after.queried);
  EXPECT_FALSE(report.selected_after.pending_id.has_value())
      << "modem legitimately reporting no pending config must show as "
         "NONE (nullopt), distinct from not-queried";
}

TEST(SktLoad, SuccessfulLoadMultipleChunks) {
  auto file = MakeSyntheticFile(2500);  // 1024 + 1024 + 452
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 2500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  uint32_t cumulative = 0;
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    cumulative += static_cast<uint32_t>(len);
    return CleanChunkOutcome(static_cast<uint32_t>(len), cumulative, total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  EXPECT_EQ(report.chunks_sent, 3u);
  EXPECT_EQ(report.bytes_sent, 2500u);
  ASSERT_EQ(fake.chunk_lens_seen.size(), 3u);
  EXPECT_EQ(fake.chunk_lens_seen[0], 1024u);
  EXPECT_EQ(fake.chunk_lens_seen[1], 1024u);
  EXPECT_EQ(fake.chunk_lens_seen[2], 452u);
}

// Directly mirrors real-device test #2: the exact stock SKT MBN size
// (41172 bytes -> 40 chunks of 1024 + one 212-byte final chunk), with
// the CONFIRMED real semantics for both Received/Remaining Size AND
// GetConfigInfo-based presence. The real device completed exactly this.
TEST(SktLoad, RealStockSktSizeAllFortyOneChunksSucceed) {
  auto file = MakeSyntheticFile(41172);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 41172);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  uint32_t cumulative = 0;
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    cumulative += static_cast<uint32_t>(len);
    return CleanChunkOutcome(static_cast<uint32_t>(len), cumulative, total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  EXPECT_EQ(report.chunks_sent, 41u);
  EXPECT_EQ(report.bytes_sent, 41172u);
  ASSERT_EQ(fake.chunk_lens_seen.size(), 41u);
  for (int i = 0; i < 40; i++) {
    EXPECT_EQ(fake.chunk_lens_seen[i], 1024u) << "chunk " << i;
  }
  EXPECT_EQ(fake.chunk_lens_seen[40], 212u) << "final (41st) chunk";
}

TEST(SktLoad, ExactBoundaryChunkSize) {
  auto file = MakeSyntheticFile(2048);  // exactly 2 * kLoadChunkSize.
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 2048);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  uint32_t cumulative = 0;
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    cumulative += static_cast<uint32_t>(len);
    return CleanChunkOutcome(static_cast<uint32_t>(len), cumulative, total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  EXPECT_EQ(report.chunks_sent, 2u)
      << "must not send a phantom empty 3rd chunk for an exact multiple";
  ASSERT_EQ(fake.chunk_lens_seen.size(), 2u);
  EXPECT_EQ(fake.chunk_lens_seen[0], 1024u);
  EXPECT_EQ(fake.chunk_lens_seen[1], 1024u);
}

TEST(SktLoad, ShortFinalChunk) {
  auto file = MakeSyntheticFile(1500);  // 1024 + 476
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 1500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  uint32_t cumulative = 0;
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    cumulative += static_cast<uint32_t>(len);
    return CleanChunkOutcome(static_cast<uint32_t>(len), cumulative, total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  ASSERT_EQ(fake.chunk_lens_seen.size(), 2u);
  EXPECT_EQ(fake.chunk_lens_seen[1], 476u);
}

TEST(SktLoad, ReceivedAbsentButValidRemainingSucceeds) {
  // "Received" is optional/advisory - libqmi's own reference client
  // never reads it at all. Its absence must never fail the transfer.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kOk;
    o.data.ok = true;
    o.data.has_received = false;  // absent entirely.
    o.data.has_remaining_size = true;
    o.data.remaining_size = total - static_cast<uint32_t>(len);
    o.data.has_frame_reset = true;
    o.data.frame_reset = false;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
}

TEST(SktLoad, ReceivedWrongForCurrentChunkFails) {
  auto file = MakeSyntheticFile(2500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    // Received claims a value that matches neither this chunk's size
    // nor any sane cumulative total - must fail regardless.
    return CleanChunkOutcome(999999, static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(fake.load_config_chunk_calls, 1);
}

TEST(SktLoad, RemainingSizeWrongFails) {
  auto file = MakeSyntheticFile(500);  // single chunk.
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t len,
                                  uint32_t) {
    // Remaining Size is authoritative and wrong here (should be 0 for a
    // single-chunk, fully-sent file, but claims bytes remain).
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kOk;
    o.data.ok = true;
    o.data.has_received = true;
    o.data.received = static_cast<uint32_t>(len);
    o.data.has_remaining_size = true;
    o.data.remaining_size = 12345;  // wrong - should be 0.
    o.data.has_frame_reset = true;
    o.data.frame_reset = false;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
}

TEST(SktLoad, FinalChunkNonzeroRemainingFails) {
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(
        static_cast<uint32_t>(len),
        1 /* wrong cumulative, forces nonzero remaining */, 500);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
}

TEST(SktLoad, ModemRejectAtFirstChunk) {
  auto file = MakeSyntheticFile(2500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t,
                                  uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kIndicationRejected;
    o.data.error_status = 7;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(fake.load_config_chunk_calls, 1)
      << "must not attempt further chunks after an outright rejection";
}

TEST(SktLoad, ModemRejectAtMiddleChunk) {
  auto file = MakeSyntheticFile(2500);  // 3 chunks
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  uint32_t cumulative = 0;
  fake.on_load_config_chunk = [&](ConfigType, const std::vector<uint8_t>&,
                                   uint32_t total, const uint8_t*, size_t len,
                                   uint32_t) {
    if (fake.load_config_chunk_calls == 2) {
      LoadConfigChunkOutcome o;
      o.status = ProbeStatus::kIndicationRejected;
      o.data.error_status = 3;
      return o;
    }
    cumulative += static_cast<uint32_t>(len);
    return CleanChunkOutcome(static_cast<uint32_t>(len), cumulative, total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(fake.load_config_chunk_calls, 2)
      << "must stop immediately, not attempt the 3rd chunk";
}

TEST(SktLoad, TimeoutBeforeIndicationGoesToUnknownResultNotRetried) {
  auto file = MakeSyntheticFile(2500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };  // diagnostic only.
  fake.on_get_config_info = AbsentThenExists(fake, 2500);  // post-timeout: EXISTS.
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kTimeout;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kUnknownResult);
  EXPECT_EQ(fake.load_config_chunk_calls, 1)
      << "a timed-out chunk must NEVER be automatically resent";
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kExists)
      << "post-timeout Get Config Info (authoritative) confirms it landed";
}

TEST(SktLoad, TimeoutThenNotFoundIsUnknownResultNotFound) {
  auto file = MakeSyntheticFile(2500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();  // both DISCOVER and post-timeout: confirmed absent.
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kTimeout;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kUnknownResult);
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kAbsent);
}

TEST(SktLoad, MalformedIndicationFails) {
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kMalformedResponse;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
}

TEST(SktLoad, FrameResetTrueIsTreatedAsFailure) {
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    auto o = CleanChunkOutcome(static_cast<uint32_t>(len),
                                static_cast<uint32_t>(len), total);
    o.data.frame_reset = true;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
}

// --- Presence-classification-specific tests (this hardening pass) ---

TEST(SktLoad, PreexistingConfigFoundByGetInfoEvenWhenListOmitsItSkipsLoad) {
  // The exact real-device scenario: List Configs OMITS the target
  // entirely, but Get Config Info on its exact id succeeds - the load
  // must still be skipped, because Get Config Info is authoritative.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };  // omits it!
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return ExistsInfo(500);  // ...but GetInfo confirms it's there.
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) {
    ADD_FAILURE() << "must not even check selected if Load is skipped "
                     "entirely at DISCOVER";
    return NoneSelected();
  };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t,
                                  uint32_t) {
    ADD_FAILURE() << "must never send a chunk if the config already exists";
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kOk;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone);
  EXPECT_TRUE(report.already_existed_before_load);
  EXPECT_EQ(report.config_presence_before, ConfigPresence::kExists);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}

TEST(SktLoad, PreloadGetInfoInvalidIdAllowsLoad) {
  // GetInfo(target) = the specific "no entry" answer -> confirmed
  // absent -> Load proceeds normally.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.config_presence_before, ConfigPresence::kAbsent);
  EXPECT_EQ(fake.load_config_chunk_calls, 1);
  EXPECT_EQ(report.final_state, LoadState::kDone);
}

TEST(SktLoad, PreloadGetInfoUnexpectedFailureFailsClosed) {
  // Anything OTHER than a confirmed EXISTS or confirmed ABSENT (no
  // entry) - e.g. a timeout - must be treated as UNKNOWN, and Load must
  // NOT proceed.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) {
    ADD_FAILURE() << "should fail before even bothering with List Configs";
    return EmptyList();
  };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return UnknownInfo();  // timeout, not a clean "no entry".
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) {
    ADD_FAILURE() << "must not proceed to selected-baseline if presence "
                     "is UNKNOWN";
    return NoneSelected();
  };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    ADD_FAILURE() << "must never load when pre-load presence is UNKNOWN";
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kOk;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(report.config_presence_before, ConfigPresence::kUnknown);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}

TEST(SktLoad, PostLoadListOmissionButGetInfoSuccessStillVerifies) {
  // The exact real-device scenario applied post-load: List Configs
  // OMITS the just-loaded target, but Get Config Info on its exact id
  // succeeds with the right size - must still be DONE.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };  // always omits it.
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kDone)
      << "Get Config Info (authoritative) confirmed presence + correct "
         "size; List Configs omitting it (diagnostic only) must not "
         "block DONE";
  EXPECT_FALSE(report.found_in_list_after);
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kExists);
}

TEST(SktLoad, PostLoadListPresentButGetInfoFailureDoesNotVerify) {
  // The inverse and, per this hardening pass, the CORRECTED behavior:
  // List Configs claiming presence is NOT sufficient. This replaces the
  // old (incorrect, pre-hardening) "List-verify-succeeds-but-Info-
  // fails-is-still-DONE" test, whose premise real-device test #2 proved
  // wrong.
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);
  auto id = Sha1Vec(file);

  FakePdcClient fake;
  fake.on_list_configs = [&](uint32_t) { return ListWithId(id); };  // claims present.
  fake.on_get_config_info = [&](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    if (fake.get_config_info_calls == 1) return AbsentInfo();  // DISCOVER.
    GetConfigInfoOutcome o;
    o.status = ProbeStatus::kTimeout;  // post-load GetInfo itself fails.
    return o;
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_NE(report.final_state, LoadState::kDone)
      << "List Configs alone (even claiming presence) must never verify "
         "success - only Get Config Info is authoritative";
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_TRUE(report.found_in_list_after);
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kUnknown);
}

TEST(SktLoad, PostMortemGetInfoRunsEvenWhenListOmitsTarget) {
  // §6: even when a chunk fails mid-transfer, a best-effort read-only
  // re-check should still run Get Config Info (authoritative) - not
  // just List Configs (diagnostic) - and populate the report, without
  // overturning the FAILED verdict the chunk failure caused.
  auto file = MakeSyntheticFile(2500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };  // always omits.
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    return AbsentInfo();  // DISCOVER + post-mortem: confirmed absent both times.
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) { return NoneSelected(); };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kIndicationRejected;
    o.data.error_status = 9;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_FALSE(report.found_in_list_after);
  EXPECT_EQ(report.config_presence_after, ConfigPresence::kAbsent)
      << "post-mortem verification must call Get Config Info (authoritative), "
         "not rely on List Configs, even though the transfer itself failed";
  EXPECT_EQ(fake.get_config_info_calls, 2)
      << "DISCOVER (1) + post-mortem re-check (2)";
  EXPECT_TRUE(report.selected_after.queried);
  EXPECT_EQ(report.selection_check, SelectionCheckResult::kUnchanged);
}

TEST(SktLoad, SelectionChangedUnexpectedlyIsTreatedAsFailure) {
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);
  auto id = Sha1Vec(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [&](ConfigType, uint32_t) {
    GetSelectedConfigOutcome o;
    o.status = ProbeStatus::kOk;
    if (fake.get_selected_config_calls == 1) {
      o.data.active_id = std::vector<uint8_t>{0xAA};  // before: Common active.
    } else {
      o.data.active_id = id;  // after: unexpectedly switched to the new SKT id!
    }
    return o;
  };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(report.selection_check, SelectionCheckResult::kChanged)
      << "Load must never implicitly change the active/pending config - "
         "this is the core C0 safety property";
}

TEST(SktLoad, SelectedAfterNotQueriedShowsNotQueriedAndFails) {
  // The exact bug found on the first real-device test: a failure to
  // re-query `selected` after the transfer must never be displayed as
  // "NONE" (which would look like the modem actively confirmed no
  // active config) - it must be visibly NOT_QUERIED, and must still be
  // treated as an unconfirmed safety invariant (kUnknown -> FAILED).
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) { return EmptyList(); };
  fake.on_get_config_info = AbsentThenExists(fake, 500);
  fake.on_get_selected_config = [&](ConfigType, uint32_t) {
    if (fake.get_selected_config_calls == 1) {
      GetSelectedConfigOutcome o;
      o.status = ProbeStatus::kOk;
      o.data.active_id = std::vector<uint8_t>{0xAA};
      return o;
    }
    GetSelectedConfigOutcome o;
    o.status = ProbeStatus::kTimeout;  // the "after" query fails outright.
    return o;
  };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t total, const uint8_t*, size_t len,
                                  uint32_t) {
    return CleanChunkOutcome(static_cast<uint32_t>(len),
                              static_cast<uint32_t>(len), total);
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_TRUE(report.selected_before.queried);
  EXPECT_FALSE(report.selected_after.queried);
  EXPECT_EQ(report.selection_check, SelectionCheckResult::kUnknown);
}

TEST(SktLoad, StockFileIdentityMismatchIsFailClosed) {
  auto file = MakeSyntheticFile(500);
  auto profile = MakeProfile(file);
  // Corrupt the expected hash so it no longer matches the real bytes -
  // simulates a tampered/wrong file being present at the known path.
  profile.expected_sha256[0] ^= 0xFF;

  FakePdcClient fake;
  fake.on_list_configs = [](uint32_t) {
    ADD_FAILURE() << "must fail closed before ever querying the modem";
    return EmptyList();
  };
  fake.on_get_config_info = [](ConfigType, const std::vector<uint8_t>&, uint32_t) {
    ADD_FAILURE() << "must fail closed before ever querying the modem";
    return AbsentInfo();
  };
  fake.on_get_selected_config = [](ConfigType, uint32_t) {
    ADD_FAILURE() << "must fail closed before ever querying the modem";
    return NoneSelected();
  };
  fake.on_load_config_chunk = [](ConfigType, const std::vector<uint8_t>&,
                                  uint32_t, const uint8_t*, size_t, uint32_t) {
    ADD_FAILURE() << "must never attempt to load an unverified file";
    LoadConfigChunkOutcome o;
    o.status = ProbeStatus::kOk;
    return o;
  };

  SktLoadReport report = RunLoadFromBytes(fake, file, profile, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_EQ(fake.get_config_info_calls, 0);
  EXPECT_EQ(fake.list_configs_calls, 0);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}

// --- Cycle 2 Section H: RunLoadCarrier() dispatches to the SAME
// already-proven RunLoadFromBytes() state machine for any of the 3
// Korean carriers - see known_profiles.h's Carrier enum. This host
// environment has none of the real /vendor/firmware_mnt/... files, so
// these tests exercise the PRECHECK-failure path (file not found) for
// each carrier - confirming the dispatcher resolves the right profile
// (distinct error messages naming the right carrier/path) and fails
// closed rather than crashing or silently proceeding, exactly like
// RunLoadSkt() already does in that same environment.
TEST(RunLoadCarrier, SktMissingFileFailsClosedWithCarrierNamedMessage) {
  FakePdcClient fake;
  SktLoadReport report = RunLoadCarrier(fake, Carrier::kSkt, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_NE(report.message.find("SKT"), std::string::npos);
  EXPECT_EQ(fake.get_config_info_calls, 0);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}

TEST(RunLoadCarrier, KtMissingFileFailsClosedWithCarrierNamedMessage) {
  FakePdcClient fake;
  SktLoadReport report = RunLoadCarrier(fake, Carrier::kKt, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_NE(report.message.find("KT"), std::string::npos);
  EXPECT_EQ(fake.get_config_info_calls, 0);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}

TEST(RunLoadCarrier, LguMissingFileFailsClosedWithCarrierNamedMessage) {
  FakePdcClient fake;
  SktLoadReport report = RunLoadCarrier(fake, Carrier::kLgu, 1000);
  EXPECT_EQ(report.final_state, LoadState::kFailed);
  EXPECT_NE(report.message.find("LGU"), std::string::npos);
  EXPECT_EQ(fake.get_config_info_calls, 0);
  EXPECT_EQ(fake.load_config_chunk_calls, 0);
}
