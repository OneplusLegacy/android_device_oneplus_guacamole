#include "pdc_client.h"
#include "qmi_tlv.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <thread>
#include <chrono>
#include <gtest/gtest.h>

using namespace guacamole_pdc;
namespace {
std::vector<uint8_t> Ind(uint32_t token, uint16_t error = 0) {
  std::vector<uint8_t> b;
  AppendTlvU32(&b, kTlvToken, token);
  uint8_t le[] = {static_cast<uint8_t>(error), static_cast<uint8_t>(error >> 8)};
  AppendTlv(&b, kTlvIndicationResult, le, sizeof(le));
  return b;
}
class FakeQcci : public IQcciShim {
 public:
  QmiIndicationCallback callback = nullptr;
  void* context = nullptr;
  int sends = 0;
  int releases = 0;
  std::vector<uint8_t> ack = {2, 4, 0, 0, 0, 0, 0};
  std::function<void(unsigned int, uint32_t)> on_send;
  bool Load(std::string*) override { return true; }
  QmiServiceObject PdcGetServiceObject(int32_t, int32_t, int32_t) override {
    return this;
  }
  int ClientInitInstance(QmiServiceObject, QmiIndicationCallback cb, void* ctx,
      uint32_t, QmiClientHandle* handle) override {
    callback = cb; context = ctx; *handle = this; return 0;
  }
  int ClientRelease(QmiClientHandle) override { ++releases; return 0; }
  void Emit(unsigned int msg, std::vector<uint8_t> bytes) {
    callback(this, msg, bytes.data(), bytes.size(), context);
    // Destroy/overwrite the callback buffer immediately: client must own its copy.
    std::fill(bytes.begin(), bytes.end(), 0xee);
  }
  int SendRawMsgSync(QmiClientHandle, unsigned int msg, const void* req,
      unsigned int len, void* response, unsigned int capacity,
      unsigned int* received, unsigned int) override {
    ++sends;
    const auto token = PeekIndicationToken(static_cast<const uint8_t*>(req), len);
    if (!token || capacity < ack.size()) return -1;
    if (on_send) on_send(msg, *token);  // deliberate indication-before-sync-ACK
    std::memcpy(response, ack.data(), ack.size());
    *received = ack.size();
    return 0;
  }
};
class PdcRouting : public testing::Test {
 protected:
  FakeQcci shim;
  PdcClient client{shim};
  void SetUp() override {
    std::string log;
    ASSERT_TRUE(client.Init({{1, 15, 6}}, &log));
  }
  SetSelectedConfigOutcome Select() {
    return client.SetSelectedConfig(ConfigType::kSoftware, {0x12, 0x34}, 1);
  }
};
}
TEST_F(PdcRouting, SetSelectedIndicationCompletesActualClient) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    EXPECT_EQ(msg, kMsgSetSelectedConfig);
    shim.Emit(msg, Ind(token));
  };
  EXPECT_EQ(Select().status, ProbeStatus::kOk);
  EXPECT_EQ(shim.sends, 1);
}
TEST_F(PdcRouting, OtherAllowedMessageWithSameTokenDoesNotCompleteSelect) {
  shim.on_send = [&](unsigned int, uint32_t token) {
    for (auto msg : {kMsgGetSelectedConfig, kMsgGetConfigInfo, kMsgListConfigs,
                     kMsgLoadConfig}) shim.Emit(msg, Ind(token));
    shim.Emit(0x0027, Ind(token));
  };
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);
  EXPECT_EQ(shim.sends, 1);
}
TEST_F(PdcRouting, WrongTokenDoesNotCompleteSelect) {
  shim.on_send = [&](unsigned int msg, uint32_t token) { shim.Emit(msg, Ind(token + 1)); };
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);
  EXPECT_EQ(shim.sends, 1);
}
TEST_F(PdcRouting, UnrelatedThenCorrectIndicationCompletes) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    shim.Emit(kMsgGetSelectedConfig, Ind(token, 41));
    shim.Emit(msg, Ind(token + 1, 41));
    shim.Emit(msg, Ind(token));
  };
  EXPECT_EQ(Select().status, ProbeStatus::kOk);
}
TEST_F(PdcRouting, ModemRejectionIsNotTimeout) {
  shim.on_send = [&](unsigned int msg, uint32_t token) { shim.Emit(msg, Ind(token, 41)); };
  const auto r = Select();
  EXPECT_EQ(r.status, ProbeStatus::kIndicationRejected);
  EXPECT_EQ(r.data.error_status, 41);
}
TEST_F(PdcRouting, CorrelatedMalformedResultFailsClosed) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    std::vector<uint8_t> b;
    AppendTlvU32(&b, kTlvToken, token);
    AppendTlvU8(&b, kTlvIndicationResult, 0);  // result must be uint16
    shim.Emit(msg, b);
  };
  EXPECT_EQ(Select().status, ProbeStatus::kMalformedResponse);
}
TEST_F(PdcRouting, DuplicateTokenFailsClosed) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    auto b = Ind(token);
    AppendTlvU32(&b, kTlvToken, token + 1);
    shim.Emit(msg, b);
  };
  EXPECT_EQ(Select().status, ProbeStatus::kMalformedResponse);
}
TEST_F(PdcRouting, DuplicateResultFailsClosed) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    auto b = Ind(token); const uint8_t err[] = {41, 0};
    AppendTlv(&b, kTlvIndicationResult, err, 2); shim.Emit(msg, b);
  };
  EXPECT_EQ(Select().status, ProbeStatus::kMalformedResponse);
}
TEST_F(PdcRouting, MissingTokenCannotBeAttributedAndTimesOut) {
  shim.on_send = [&](unsigned int msg, uint32_t) { shim.Emit(msg, {1, 2, 0, 0, 0}); };
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);
}
TEST_F(PdcRouting, MissingIndicationTimesOutWithoutRetry) {
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);
  EXPECT_EQ(shim.sends, 1);
}
TEST_F(PdcRouting, SyncRejectionWinsEvenIfIndicationArrived) {
  shim.ack = {2, 4, 0, 1, 0, 48, 0};
  shim.on_send = [&](unsigned int msg, uint32_t token) { shim.Emit(msg, Ind(token)); };
  EXPECT_EQ(Select().status, ProbeStatus::kRequestRejected);
  shim.ack = {2, 4, 0, 0, 0, 0, 0}; shim.on_send = nullptr;
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);  // no stale success
}
TEST_F(PdcRouting, MalformedSyncAckFailsClosed) {
  shim.ack = {2, 4, 0, 0};
  EXPECT_EQ(Select().status, ProbeStatus::kMalformedResponse);
  EXPECT_EQ(shim.sends, 1);
}
TEST_F(PdcRouting, LateIndicationAndFutureTokenAreNotCached) {
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);  // token 1
  shim.Emit(kMsgSetSelectedConfig, Ind(1));
  shim.Emit(kMsgSetSelectedConfig, Ind(2));
  EXPECT_EQ(Select().status, ProbeStatus::kTimeout);  // explicit second host call
  EXPECT_EQ(shim.sends, 2);
}
TEST_F(PdcRouting, FirstIndicationCannotBeOverwritten) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    shim.Emit(msg, Ind(token, 41)); shim.Emit(msg, Ind(token));
  };
  EXPECT_EQ(Select().status, ProbeStatus::kIndicationRejected);
}
TEST_F(PdcRouting, ListGetInfoGetSelectedLoadStillRoute) {
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    shim.Emit(kMsgSetSelectedConfig, Ind(token, 41));  // wrong message, same token
    auto b = Ind(token);
    if (msg == kMsgLoadConfig) {
      AppendTlvU32(&b, kTlvLoadConfigIndReceived, 1);
      AppendTlvU32(&b, kTlvLoadConfigIndRemainingSize, 0);
    }
    shim.Emit(msg, b);
  };
  EXPECT_EQ(client.ListConfigs(1).status, ProbeStatus::kOk);
  EXPECT_EQ(client.GetConfigInfo(ConfigType::kSoftware, {1}, 1).status, ProbeStatus::kOk);
  EXPECT_EQ(client.GetSelectedConfig(ConfigType::kSoftware, 1).status, ProbeStatus::kOk);
  const uint8_t chunk = 0x12;
  EXPECT_EQ(client.LoadConfigChunk(ConfigType::kSoftware, {1}, 1, &chunk, 1, 1).status,
            ProbeStatus::kOk);
  EXPECT_EQ(shim.sends, 4);
}

TEST_F(PdcRouting, InvalidRequestTypeOrEmptyIdNeverSends) {
  EXPECT_EQ(client.SetSelectedConfig(static_cast<ConfigType>(99), {1}, 1).status,
            ProbeStatus::kMalformedResponse);
  EXPECT_EQ(client.SetSelectedConfig(ConfigType::kSoftware, {}, 1).status,
            ProbeStatus::kMalformedResponse);
  EXPECT_EQ(shim.sends, 0);
}

TEST_F(PdcRouting, AsyncIndicationAfterSyncAckCompletesWaitingClient) {
  std::thread delivery;
  shim.on_send = [&](unsigned int msg, uint32_t token) {
    delivery = std::thread([&, msg, token] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      shim.Emit(msg, Ind(token));
    });
  };
  const auto r = client.SetSelectedConfig(ConfigType::kSoftware, {1}, 1000);
  if (delivery.joinable()) delivery.join();
  EXPECT_EQ(r.status, ProbeStatus::kOk);
  EXPECT_EQ(shim.sends, 1);
}
