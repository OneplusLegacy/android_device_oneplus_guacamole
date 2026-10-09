#include "callback_smoke.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "diag_callback_shim.h"
#include "diag_loopback_packet.h"

using namespace guacamole_efs;
using namespace std::chrono_literals;

namespace {

class FakeShim : public IDiagCallbackShim {
 public:
  bool init_result = true;
  DiagDataCallback captured_callback = nullptr;
  void* captured_context = nullptr;
  int last_send_proc = -1;
  std::vector<uint8_t> last_send_bytes;
  std::function<void()> on_send;

  bool Init(std::string*) override { return init_result; }
  void Deinit() override {}
  bool RegisterCallback(DiagDataCallback cb, void* context,
                         std::string*) override {
    captured_callback = cb;
    captured_context = context;
    return true;
  }
  bool SwitchLogging(int, std::string*) override { return true; }
  bool SendData(int proc, const void* buf, int len, std::string*) override {
    last_send_proc = proc;
    last_send_bytes.assign(static_cast<const uint8_t*>(buf),
                            static_cast<const uint8_t*>(buf) + len);
    if (on_send) on_send();
    return true;
  }
};

void Fire(FakeShim* shim, std::vector<uint8_t> data) {
  ASSERT_NE(shim->captured_callback, nullptr);
  shim->captured_callback(data.data(), static_cast<int>(data.size()),
                           shim->captured_context);
}

}  // namespace

// A matching response's first 4 bytes must echo the request's own
// header (0x4B, 0x12, 0x29, 0x00 - kH41LoopbackPacket's own header) -
// see DiagTransport's response-correlation fix. An earlier revision of
// this test fired `{0x4B, 0x99, 0x01}` (a header that does NOT match
// the loopback request at all) and still asserted `kOk`, which a review
// correctly flagged as a test whose name ("matching") contradicted what
// it actually verified.
std::vector<uint8_t> MatchingLoopbackResponse(std::vector<uint8_t> payload) {
  std::vector<uint8_t> resp(kH41LoopbackPacket.begin(),
                             kH41LoopbackPacket.begin() + 4);
  resp.insert(resp.end(), payload.begin(), payload.end());
  return resp;
}

TEST(CallbackSmoke, SendsExactH41LoopbackPacketBytes) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, MatchingLoopbackResponse({0x00})); };
  RunCallbackSmoke(&t, 500ms);

  std::vector<uint8_t> expected(kH41LoopbackPacket.begin(),
                                 kH41LoopbackPacket.end());
  EXPECT_EQ(fake->last_send_bytes, expected);
  EXPECT_EQ(fake->last_send_proc, 0);
}

TEST(CallbackSmoke, ReturnsOkOnMatchingResponse) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> matching_response = MatchingLoopbackResponse({0xAA, 0xBB});
  fake->on_send = [&] { Fire(fake, matching_response); };

  auto r = RunCallbackSmoke(&t, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, matching_response);
}

TEST(CallbackSmoke, UnrelatedCallbackIgnoredThenMatchingResponseSucceeds) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> matching_response = MatchingLoopbackResponse({0x01});
  fake->on_send = [&] {
    Fire(fake, {0x4B, 0x99, 0x01, 0x00});  // unrelated - wrong header.
    Fire(fake, matching_response);
  };

  auto r = RunCallbackSmoke(&t, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, matching_response);
}

TEST(CallbackSmoke, UnrelatedOnlyResponseTimesOut) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  // A response with a completely different header must NOT be accepted
  // as this loopback request's answer.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x99, 0x01, 0x00}); };

  auto r = RunCallbackSmoke(&t, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(CallbackSmoke, LoopbackPacketIsExactlySevenBytes) {
  // Regression guard tied directly to the disassembly citation in
  // diag_loopback_packet.h - if this array's contents ever drift from
  // what was traced there, this test (and the exact-bytes test above)
  // catches it.
  EXPECT_EQ(kH41LoopbackPacket.size(), 7u);
  std::vector<uint8_t> expected = {0x4B, 0x12, 0x29, 0x00, 0x01, 0x02, 0x03};
  EXPECT_EQ(std::vector<uint8_t>(kH41LoopbackPacket.begin(),
                                  kH41LoopbackPacket.end()),
            expected);
}

TEST(CallbackSmoke, TimeoutDoesNotRetry) {
  auto owned = std::make_unique<FakeShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));
  // No callback ever fires.

  auto r = RunCallbackSmoke(&t, 60ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}
