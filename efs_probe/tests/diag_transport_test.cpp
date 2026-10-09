#include "diag_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "diag_frame.h"
#include "diag_loopback_packet.h"
#include "efs_wire.h"

using namespace guacamole_efs;
using namespace std::chrono_literals;

namespace {

// Fake IDiagCallbackShim: fully scripted, records call counts, and lets
// a test capture and later invoke the registered callback directly -
// simulating what the real libdiag read thread does asynchronously.
// `captured_context` mirrors the REAL contract corrected 2026-09-23:
// diag_register_callback's second argument is stored and echoed back
// verbatim as the callback's third argument (see diag_callback_shim.h)
// - so this fake stores it exactly the same way, and Fire() replays it.
class FakeDiagCallbackShim : public IDiagCallbackShim {
 public:
  bool init_result = true;
  bool register_result = true;
  bool switch_logging_result = true;
  bool send_result = true;

  int init_calls = 0;
  int deinit_calls = 0;
  int register_calls = 0;
  int switch_calls = 0;
  int send_calls = 0;

  int last_switch_mode = -1;
  int last_send_proc = -1;
  std::vector<uint8_t> last_send_bytes;

  DiagDataCallback captured_callback = nullptr;
  void* captured_context = nullptr;

  // Ordered record of every shim call, e.g. "SwitchLogging(0)" before
  // "Deinit" - lets a test assert call ORDER, not just counts.
  std::vector<std::string> call_log;

  // Test hooks: invoked synchronously from inside RegisterCallback()/
  // SendData(), right after recording the call - let a test simulate a
  // callback firing "during" the call, or coordinate with another
  // thread to reproduce a specific interleaving.
  std::function<void()> on_register;
  std::function<void()> on_send;

  bool Init(std::string* error) override {
    init_calls++;
    call_log.push_back("Init");
    if (!init_result && error) *error = "fake init failure";
    return init_result;
  }
  void Deinit() override {
    deinit_calls++;
    call_log.push_back("Deinit");
  }

  bool RegisterCallback(DiagDataCallback cb, void* context,
                         std::string* error) override {
    register_calls++;
    call_log.push_back("RegisterCallback");
    captured_callback = cb;
    captured_context = context;
    if (on_register) on_register();
    if (!register_result && error) *error = "fake register failure";
    return register_result;
  }

  bool SwitchLogging(int mode, std::string* error) override {
    switch_calls++;
    last_switch_mode = mode;
    call_log.push_back("SwitchLogging(" + std::to_string(mode) + ")");
    if (!switch_logging_result && error) *error = "fake switch failure";
    return switch_logging_result;
  }

  bool SendData(int proc, const void* buf, int len,
                std::string* error) override {
    send_calls++;
    last_send_proc = proc;
    call_log.push_back("SendData");
    last_send_bytes.assign(static_cast<const uint8_t*>(buf),
                            static_cast<const uint8_t*>(buf) + len);
    if (on_send) on_send();
    if (!send_result && error) *error = "fake send failure";
    return send_result;
  }
};

// Fires the captured callback exactly as the real libdiag read thread
// would - a raw buffer, a length, and the SAME context pointer that was
// registered (never something a test makes up independently).
void Fire(FakeDiagCallbackShim* shim, std::vector<uint8_t> data) {
  ASSERT_NE(shim->captured_callback, nullptr);
  shim->captured_callback(data.data(), static_cast<int>(data.size()),
                           shim->captured_context);
}

// Wraps `payload` in the H41_DEVICE_CONFIRMED HDLC-style framing (see
// diag_frame.h): a correct little-endian CRC-16/X-25 followed by the
// 0x7e terminator - simulating exactly what a real H.41 callback
// response looks like, for the framing-normalization tests below.
std::vector<uint8_t> FramedResponse(std::vector<uint8_t> payload) {
  uint16_t crc = Crc16X25(payload.data(), payload.size());
  payload.push_back(static_cast<uint8_t>(crc & 0xFF));
  payload.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
  payload.push_back(0x7e);
  return payload;
}

}  // namespace

// --- Init/Deinit ---

TEST(DiagTransport, InitFailureFailsClosed) {
  auto fake = std::make_unique<FakeDiagCallbackShim>();
  fake->init_result = false;
  DiagTransport t(std::move(fake), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  EXPECT_FALSE(t.Init(&error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(t.initialized());
}

TEST(DiagTransport, InitSuccessAndDeinit) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));
  EXPECT_TRUE(t.initialized());
  t.Deinit();
  EXPECT_FALSE(t.initialized());
  EXPECT_EQ(fake->deinit_calls, 1);
}

// --- Callback-readiness failures (register/switch-logging) ---

TEST(DiagTransport, CallbackRegistrationFailureFailsClosed) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  fake->register_result = false;
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r = t.SendSubsysCommand(0, {0x4B}, 50ms);
  EXPECT_EQ(r.status, TransportStatus::kRegisterCallbackFailed);
  EXPECT_EQ(fake->send_calls, 0);
}

TEST(DiagTransport, SwitchLoggingFailureFailsClosed) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  fake->switch_logging_result = false;
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r = t.SendSubsysCommand(0, {0x4B}, 50ms);
  EXPECT_EQ(r.status, TransportStatus::kSwitchLoggingFailed);
  EXPECT_EQ(fake->send_calls, 0);
}

TEST(DiagTransport, SendFailureFailsClosed) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  fake->send_result = false;
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r = t.SendSubsysCommand(0, {0x4B}, 50ms);
  EXPECT_EQ(r.status, TransportStatus::kSendFailed);
  EXPECT_EQ(fake->send_calls, 1);

  // The request slot is released, but uncertain send failure poisons the session.
  fake->send_result = true;
  fake->on_send = [&] { Fire(fake, {0xAA}); };
  auto r2 = t.SendSubsysCommand(0, {0x4B}, 200ms);
  EXPECT_EQ(r2.status, TransportStatus::kSessionDesynchronized);
}

// --- Round trip / correlation ---

TEST(DiagTransport, SingleRequestResponseRoundTrip) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> expected_response = {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3};
  fake->on_send = [&] { Fire(fake, expected_response); };

  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, expected_response);
  EXPECT_EQ(fake->last_send_proc, 0);
  EXPECT_EQ(fake->register_calls, 1);
  EXPECT_EQ(fake->switch_calls, 1);
  EXPECT_EQ(fake->last_switch_mode, kDiagCallbackMode);

  // A second, independent round trip must also work (state correctly
  // returns to kIdle, RegisterCallback/SwitchLogging are NOT repeated).
  fake->on_send = [&] { Fire(fake, {0xCC}); };
  auto r2 = t.SendSubsysCommand(0, {0x01}, 500ms);
  ASSERT_EQ(r2.status, TransportStatus::kOk);
  EXPECT_EQ(r2.response, (std::vector<uint8_t>{0xCC}));
  EXPECT_EQ(fake->register_calls, 1);
  EXPECT_EQ(fake->switch_calls, 1);
}

TEST(DiagTransport, SecondConcurrentRequestRejected) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::mutex m;
  std::condition_variable cv;
  bool send_started = false;
  fake->on_send = [&] {
    std::lock_guard<std::mutex> lock(m);
    send_started = true;
    cv.notify_one();
  };

  std::thread worker([&] {
    // Long timeout - this call blocks until the main thread fires the
    // callback below.
    auto r = t.SendSubsysCommand(0, {0x4B}, 5000ms);
    EXPECT_EQ(r.status, TransportStatus::kOk);
  });

  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return send_started; }));
  }

  // The worker thread's request is now outstanding (SendData has been
  // called, no callback fired yet). A second call must be rejected
  // immediately, without ever touching the shim again.
  auto r2 = t.SendSubsysCommand(1, {0xFF}, 50ms);
  EXPECT_EQ(r2.status, TransportStatus::kAlreadyOutstanding);
  EXPECT_EQ(fake->send_calls, 1);  // still just the worker's one call.

  Fire(fake, {0x4B, 0x00});  // let the worker's request complete.
  worker.join();
}

// Regression test for the concurrency bug a code review found in the
// original implementation: the reservation check (`state_ == kWaiting`)
// and the reservation itself (`state_ = kWaiting`) were NOT atomic -
// two threads could both pass the check before either reserved the
// slot, both reach RegisterCallback/SendData, and then share the SAME
// state_/result_bytes_, letting one thread's timeout complete (as
// "OK") the OTHER thread's call with no matching callback ever sent.
// This test races a second call in at the EARLIEST possible point -
// while the first call is still inside RegisterCallback(), before
// SwitchLogging or SendData have run at all - which the OLD code would
// not have rejected (request_in_flight_ now must reject it).
TEST(DiagTransport, ConcurrentEntryBeforeReservationRejectsSecond) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::mutex m;
  std::condition_variable cv;
  bool entered_register = false;
  bool release_a = false;

  fake->on_register = [&] {
    std::unique_lock<std::mutex> lock(m);
    entered_register = true;
    cv.notify_one();
    cv.wait(lock, [&] { return release_a; });
  };
  fake->on_send = [&] { Fire(fake, {0x01}); };

  TransportStatus a_status = TransportStatus::kInternalStateError;
  std::thread a([&] {
    auto r = t.SendSubsysCommand(0, {0x4B}, 2000ms);
    a_status = r.status;
  });

  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return entered_register; }));
  }

  // Thread A is blocked INSIDE RegisterCallback right now - before
  // SwitchLogging or SendData have ever been called. A second call must
  // still be rejected immediately and must never reach the shim.
  auto b = t.SendSubsysCommand(0, {0xFF}, 50ms);
  EXPECT_EQ(b.status, TransportStatus::kAlreadyOutstanding);
  EXPECT_EQ(fake->register_calls, 1);
  EXPECT_EQ(fake->switch_calls, 0);
  EXPECT_EQ(fake->send_calls, 0);

  {
    std::lock_guard<std::mutex> lock(m);
    release_a = true;
  }
  cv.notify_one();
  a.join();
  EXPECT_EQ(a_status, TransportStatus::kOk);
}

TEST(DiagTransport, NullCallbackRejected) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    ASSERT_NE(fake->captured_callback, nullptr);
    fake->captured_callback(nullptr, 5, fake->captured_context);
  };
  auto r = t.SendSubsysCommand(0, {0x4B}, 100ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, ZeroLengthCallbackRejected) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, {}); };
  auto r = t.SendSubsysCommand(0, {0x4B}, 100ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, OversizedCallbackRejected) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    std::vector<uint8_t> huge(9000, 0xAB);  // > kMaxCallbackBytes (8192)
    Fire(fake, huge);
  };
  auto r = t.SendSubsysCommand(0, {0x4B}, 100ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

// --- Timeout / late / unsolicited / duplicate ---

TEST(DiagTransport, TimeoutDoesNotRetry) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));
  // No callback ever fires.

  auto r = t.SendSubsysCommand(0, {0x4B}, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
  EXPECT_EQ(fake->send_calls, 1);  // exactly one attempt - no retry.
}

TEST(DiagTransport, NoCallbackCanNeverProduceOk) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r = t.SendSubsysCommand(0, {0x01}, 60ms);
  EXPECT_NE(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, LateCallbackAfterTimeoutIgnored) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r = t.SendSubsysCommand(0, {0x4B}, 80ms);
  ASSERT_EQ(r.status, TransportStatus::kTimeout);

  // A stray callback arrives after the timeout was already declared.
  Fire(fake, {0xDE, 0xAD});

  // A timeout now poisons the session (see TimeoutPoisonsSession) - the
  // caller must explicitly restart it before sending again. It must not
  // have completed/corrupted anything either way - a fresh request
  // after reinit gets its OWN, correctly-matched response, not the
  // stale bytes.
  t.Deinit();
  ASSERT_TRUE(t.Init(&error));
  fake->on_send = [&] { Fire(fake, {0x99}); };
  auto r2 = t.SendSubsysCommand(0, {0x01}, 200ms);
  ASSERT_EQ(r2.status, TransportStatus::kOk);
  EXPECT_EQ(r2.response, (std::vector<uint8_t>{0x99}));
}

TEST(DiagTransport, OneRequestTimeoutCannotCompleteAnotherRequest) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  // First request times out - no callback ever fires for it.
  auto r1 = t.SendSubsysCommand(0, {0x01}, 60ms);
  ASSERT_EQ(r1.status, TransportStatus::kTimeout);

  // A stray callback "from" the first request arrives late.
  Fire(fake, {0xDE, 0xAD});

  // The timeout poisoned the session - restart it explicitly (see
  // TimeoutPoisonsSession), then the second, independent request must
  // get its OWN correct answer - never silently completed by the first
  // request's timeout or its late callback (the exact bug the pre-fix
  // code exhibited when two requests shared unsynchronized state).
  t.Deinit();
  ASSERT_TRUE(t.Init(&error));
  fake->on_send = [&] { Fire(fake, {0x42}); };
  auto r2 = t.SendSubsysCommand(0, {0x02}, 200ms);
  ASSERT_EQ(r2.status, TransportStatus::kOk);
  EXPECT_EQ(r2.response, (std::vector<uint8_t>{0x42}));
}

TEST(DiagTransport, UnsolicitedCallbackDoesNotCompleteRequest) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  // One real round trip first, so a callback is actually registered
  // (captured_callback/captured_context populated) and the transport
  // returns to kIdle.
  fake->on_send = [&] { Fire(fake, {0x01}); };
  auto r0 = t.SendSubsysCommand(0, {0x4B}, 200ms);
  ASSERT_EQ(r0.status, TransportStatus::kOk);

  // Now fire a callback with NO request outstanding at all.
  Fire(fake, {0xEE, 0xEE});

  // A subsequent, unrelated request must still work normally and get
  // its OWN response - the unsolicited callback didn't leave any
  // dangling "completed" state behind.
  fake->on_send = [&] { Fire(fake, {0x77}); };
  auto r1 = t.SendSubsysCommand(0, {0x02}, 200ms);
  ASSERT_EQ(r1.status, TransportStatus::kOk);
  EXPECT_EQ(r1.response, (std::vector<uint8_t>{0x77}));
}

TEST(DiagTransport, DuplicateCallbackDoesNotCompleteTwice) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    Fire(fake, {0x01});  // first - this one should "win".
    Fire(fake, {0x02});  // duplicate - must be dropped.
  };
  auto r = t.SendSubsysCommand(0, {0x4B}, 200ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, (std::vector<uint8_t>{0x01}));
}

// --- Callback context lifetime / routing ---

TEST(DiagTransport, NoDanglingStackContext) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, {0x01}); };
  auto r = t.SendSubsysCommand(0, {0x4B}, 200ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);

  // The registered context must be the DiagTransport instance itself -
  // a pointer whose lifetime is the caller's object, never a local
  // variable inside RegisterCallback()/SendSubsysCommand() (which would
  // already be out of scope by the time this assertion runs). This is
  // the direct regression test for the dangling-stack-pointer bug a
  // code review found in the original implementation.
  EXPECT_EQ(fake->captured_context, static_cast<void*>(&t));
}

TEST(DiagTransport, CallbackContextRemainsValidAfterRegisterReturns) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, {0x07}); };
  auto r = t.SendSubsysCommand(0, {0x01}, 200ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);

  // RegisterCallback() (the function call, and every stack frame it
  // used) has long since returned by now. Firing the SAME captured
  // context again for a fresh request must work correctly - proving
  // the context pointer is still valid, not merely "happened not to
  // crash once".
  fake->on_send = [&] { Fire(fake, {0x08}); };
  auto r2 = t.SendSubsysCommand(0, {0x02}, 200ms);
  ASSERT_EQ(r2.status, TransportStatus::kOk);
  EXPECT_EQ(r2.response, (std::vector<uint8_t>{0x08}));
}

TEST(DiagTransport, CallbackRoutesToCorrectTransportInstance) {
  auto owned_a = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake_a = owned_a.get();
  DiagTransport a(std::move(owned_a), DiagFrameMode::kTestAllowUnframed);

  auto owned_b = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake_b = owned_b.get();
  DiagTransport b(std::move(owned_b), DiagFrameMode::kTestAllowUnframed);

  std::string error;
  ASSERT_TRUE(a.Init(&error));
  ASSERT_TRUE(b.Init(&error));

  std::mutex m;
  std::condition_variable cv;
  bool a_registered = false;
  fake_a->on_register = [&] {
    std::lock_guard<std::mutex> lock(m);
    a_registered = true;
    cv.notify_one();
  };

  TransportStatus a_status = TransportStatus::kInternalStateError;
  std::thread ta([&] {
    auto r = a.SendSubsysCommand(0, {0x01}, 2000ms);
    a_status = r.status;
  });

  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return a_registered; }));
  }

  // `a`'s request is still outstanding (no callback fired for it yet).
  // A completely independent round trip on `b` - a DIFFERENT
  // DiagTransport instance with its own context - must work normally
  // and must not be affected by `a`'s in-flight request in any way.
  fake_b->on_send = [&] { Fire(fake_b, {0xBB}); };
  auto b_result = b.SendSubsysCommand(0, {0x02}, 500ms);
  ASSERT_EQ(b_result.status, TransportStatus::kOk);
  EXPECT_EQ(b_result.response, (std::vector<uint8_t>{0xBB}));
  EXPECT_NE(fake_a->captured_context, fake_b->captured_context);

  // Completing `a`'s request now must route to `a`, not `b` (which has
  // already finished and is not waiting on anything).
  ASSERT_NE(fake_a->captured_context, nullptr);
  Fire(fake_a, {0xAA});
  ta.join();
  EXPECT_EQ(a_status, TransportStatus::kOk);
}

// --- Shutdown ---

TEST(DiagTransport, ResponseCopiedBeforeCallbackReturns) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    std::vector<uint8_t> buf = {0x11, 0x22, 0x33};
    Fire(fake, buf);
    // Mutate the ORIGINAL buffer after the callback has returned - if
    // OnCallback merely took a pointer instead of copying, this would
    // corrupt the eventual result.
    for (auto& b : buf) b = 0x00;
  };

  auto r = t.SendSubsysCommand(0, {0x4B}, 200ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, (std::vector<uint8_t>{0x11, 0x22, 0x33}));
}

TEST(DiagTransport, ShutdownCancelsPendingRequest) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::mutex m;
  std::condition_variable cv;
  bool send_started = false;
  fake->on_send = [&] {
    std::lock_guard<std::mutex> lock(m);
    send_started = true;
    cv.notify_one();
  };

  TransportStatus worker_status = TransportStatus::kOk;
  std::thread worker([&] {
    // A very long timeout - only Deinit() should be able to end this
    // promptly.
    auto r = t.SendSubsysCommand(0, {0x4B}, 60000ms);
    worker_status = r.status;
  });

  {
    std::unique_lock<std::mutex> lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return send_started; }));
  }

  t.Deinit();
  worker.join();
  EXPECT_EQ(worker_status, TransportStatus::kCancelled);
}

// --- Response correlation (a review found the original OnCallback()
// accepted the FIRST valid-length callback unconditionally, with no
// check that it actually answered the outstanding request) ---

TEST(DiagTransport, UnrelatedCallbackDoesNotCompleteLoopbackRequest) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request(kH41LoopbackPacket.begin(),
                                kH41LoopbackPacket.end());
  // Wrong subsys (0x99) and cmd (0x01) - not the loopback request's own
  // 0x12/0x0029 header.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x99, 0x01, 0x00}); };
  auto r = t.SendSubsysCommand(0, request, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, UnrelatedCallbackThenMatchingResponseSucceeds) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  std::vector<uint8_t> matching_response = {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3};
  fake->on_send = [&] {
    Fire(fake, {0x4B, 0x99, 0x01, 0x00});  // unrelated - must be discarded.
    Fire(fake, matching_response);
  };
  auto r = t.SendSubsysCommand(0, request, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, matching_response);
}

TEST(DiagTransport, OnlyUnrelatedCallbacksEventuallyTimeout) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  fake->on_send = [&] {
    Fire(fake, {0x4B, 0x99, 0x01, 0x00});
    Fire(fake, {0x4B, 0x02, 0x00, 0x00});
  };
  auto r = t.SendSubsysCommand(0, request, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, MatchingSubsystemWrongCommandIsIgnored) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  // Same magic+subsys (0x4B, 0x13), different cmd (0x10 vs 0x0F).
  fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x10, 0x00}); };
  auto r = t.SendSubsysCommand(0, request, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, MatchingCommandWrongSubsystemIsIgnored) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  // Same magic+cmd, different subsys (0x14 vs 0x13).
  fake->on_send = [&] { Fire(fake, {0x4B, 0x14, 0x0F, 0x00}); };
  auto r = t.SendSubsysCommand(0, request, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, TooShortCallbackIsIgnored) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  // Only 2 bytes - too short to even carry a 4-byte header.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x13}); };
  auto r = t.SendSubsysCommand(0, request, 80ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);
}

TEST(DiagTransport, TimeoutIsMeasuredFromOriginalSendNotLastUnrelatedCallback) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  std::thread unrelated_sender;
  fake->on_send = [&] {
    unrelated_sender = std::thread([fake] {
      for (int i = 0; i < 3; i++) {
        std::this_thread::sleep_for(40ms);
        Fire(fake, {0x4B, 0x99, 0x01, 0x00});
      }
    });
  };

  auto start = std::chrono::steady_clock::now();
  auto r = t.SendSubsysCommand(0, request, 100ms);
  auto elapsed = std::chrono::steady_clock::now() - start;
  unrelated_sender.join();

  EXPECT_EQ(r.status, TransportStatus::kTimeout);
  // Must time out close to the ORIGINAL 100ms deadline, not be pushed
  // out by unrelated callbacks arriving at ~40/80/120ms after send.
  EXPECT_LT(elapsed, 250ms);
}

// --- Timeout poisons the session (no transaction token exists at this
// protocol layer, so a late response to a timed-out request could
// otherwise be mistaken for a later, unrelated request's own answer) ---

TEST(DiagTransport, TimeoutPoisonsSession) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r1 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 60ms);
  ASSERT_EQ(r1.status, TransportStatus::kTimeout);

  auto r2 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 60ms);
  EXPECT_EQ(r2.status, TransportStatus::kSessionDesynchronized);
}

TEST(DiagTransport, CommandAfterTimeoutRejectedUntilReinit) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r1 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 60ms);
  ASSERT_EQ(r1.status, TransportStatus::kTimeout);

  int send_calls_before = fake->send_calls;
  auto r2 = t.SendSubsysCommand(0, {0x02}, 60ms);
  auto r3 = t.SendSubsysCommand(0, {0x03}, 60ms);
  EXPECT_EQ(r2.status, TransportStatus::kSessionDesynchronized);
  EXPECT_EQ(r3.status, TransportStatus::kSessionDesynchronized);
  EXPECT_EQ(fake->send_calls, send_calls_before);  // shim never touched.
}

TEST(DiagTransport, ReinitAfterTimeoutAllowsFreshCommand) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto r1 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 60ms);
  ASSERT_EQ(r1.status, TransportStatus::kTimeout);

  t.Deinit();
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x0F, 0x00, 0xAA}); };
  auto r2 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  EXPECT_EQ(r2.status, TransportStatus::kOk);
}

TEST(DiagTransport, LateSameCommandResponseCannotCompleteNewRequest) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> request = {0x4B, 0x13, 0x0F, 0x00};
  auto r1 = t.SendSubsysCommand(0, request, 60ms);
  ASSERT_EQ(r1.status, TransportStatus::kTimeout);

  t.Deinit();
  ASSERT_TRUE(t.Init(&error));

  // A NEW request, same subsys/cmd header as the timed-out one.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x0F, 0x00, 0xBB}); };
  auto r2 = t.SendSubsysCommand(0, request, 500ms);
  ASSERT_EQ(r2.status, TransportStatus::kOk);

  // The STALE response to request #1 (identical header!) arrives late,
  // well after r2 already completed. It must be dropped harmlessly, not
  // retroactively touch r2's already-returned result.
  Fire(fake, {0x4B, 0x13, 0x0F, 0x00, 0xCC});
  EXPECT_EQ(r2.response, (std::vector<uint8_t>{0x4B, 0x13, 0x0F, 0x00, 0xBB}));
}

// --- Deinit's logging-mode cleanup order ---
//
// CORRECTED 2026-09-23 after a real-device `callback-smoke` run: an
// earlier version of DiagTransport::Deinit() called SwitchLogging(0)
// before Diag_LSM_DeInit(), assuming mode 0 is a valid "logging off"
// cleanup mode. The real device rejected it (diag_switch_logging
// logged "unable to switch logging mode to 0, err: -1, errno: 22"; the
// kernel independently logged "request to switch to invalid mode: 0" -
// cross-checked against the public Android diag kernel driver's own
// diag_switch_logging(), whose mode-validation switch statement simply
// does not include 0 among CALLBACK_MODE/UART_MODE/SOCKET_MODE/
// MEMORY_DEVICE_MODE/USB_MODE). Deinit() no longer calls SwitchLogging
// at all - the two tests below replace the old
// DeinitDisablesCallbackLoggingBeforeLsmDeinit/
// DeinitWithoutEverSendingSkipsLoggingCleanup pair, whose premise (mode
// 0 as a valid cleanup step) this real-device result disproved.

TEST(DiagTransport, DeinitAfterCallbackSessionCallsLsmDeinit) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x0F, 0x00}); };
  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 200ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);

  t.Deinit();

  // Diag_LSM_DeInit() must still run after a real callback-mode
  // session, matching the real device (it internally does its own
  // pthread_kill -> pthread_join -> close -> free cleanup, independent
  // of any logging-mode switch - confirmed via disassembly).
  EXPECT_EQ(fake->deinit_calls, 1);
}

TEST(DiagTransport, NoInvalidLoggingModeZeroIsRequested) {
  // Neither a plain Init/Deinit (transport-info's own shape) nor a
  // Deinit() after a real callback session may ever ask for mode 0 -
  // it is not a valid mode on this firmware (H41_DEVICE_CONFIRMED).
  {
    auto owned = std::make_unique<FakeDiagCallbackShim>();
    FakeDiagCallbackShim* fake = owned.get();
    DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
    std::string error;
    ASSERT_TRUE(t.Init(&error));
    t.Deinit();
    EXPECT_EQ(fake->switch_calls, 0);
  }
  {
    auto owned = std::make_unique<FakeDiagCallbackShim>();
    FakeDiagCallbackShim* fake = owned.get();
    DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
    std::string error;
    ASSERT_TRUE(t.Init(&error));
    fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x0F, 0x00}); };
    auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 200ms);
    ASSERT_EQ(r.status, TransportStatus::kOk);
    t.Deinit();
    // Exactly the one CALLBACK_MODE(6) switch from SendSubsysCommand's
    // own readiness setup - never a second switch to mode 0.
    EXPECT_EQ(fake->switch_calls, 1);
    EXPECT_EQ(fake->last_switch_mode, kDiagCallbackMode);
  }
}

// --- 2026-09-23 preflight finding: H.41 callback responses carry a
// trailing HDLC-style CRC-16/X-25 + 0x7e terminator that
// SendSubsysCommand() must normalize away centrally - see
// diag_frame.h. ---

TEST(DiagTransport, FramedResponseNormalizedToLogicalWithRawPreserved) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> payload = {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3};
  std::vector<uint8_t> raw = FramedResponse(payload);
  fake->on_send = [&] { Fire(fake, raw); };

  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_EQ(r.response, payload);        // logical - de-framed.
  EXPECT_EQ(r.raw_response, raw);        // raw - untouched, still framed.
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_TRUE(r.crc_valid);
}

TEST(DiagTransport, LogicalHeaderCorrelationOccursAfterNormalization) {
  // The response correlation check inside OnCallback() itself compares
  // the RAW callback's first 4 bytes against the request's own header -
  // which still works unchanged, since the echo header is always at
  // the FRONT of the buffer regardless of a trailing CRC/terminator.
  // This test confirms a framed response with the WRONG echoed header
  // is still correctly rejected as unrelated (times out), proving
  // correlation isn't accidentally bypassed or broken by framing.
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> wrong_header_payload = {0x4B, 0x99, 0x99, 0x00, 1};
  fake->on_send = [&] { Fire(fake, FramedResponse(wrong_header_payload)); };

  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 100ms);
  EXPECT_EQ(r.status, TransportStatus::kTimeout);  // unrelated, discarded.
}

TEST(DiagTransport, CrcMismatchPoisonsSession) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> payload = {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3};
  std::vector<uint8_t> raw = payload;
  raw.push_back(0x00);  // deliberately wrong CRC bytes.
  raw.push_back(0x00);
  raw.push_back(0x7e);
  fake->on_send = [&] { Fire(fake, raw); };

  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kFrameIntegrityFailed);
  EXPECT_TRUE(r.had_hdlc_trailer);
  EXPECT_FALSE(r.crc_valid);
  EXPECT_EQ(r.raw_response, raw);
  EXPECT_TRUE(r.response.empty());  // never normalized - no logical bytes.

  // Session must now be poisoned, exactly like a timeout - no automatic
  // recovery, every further call rejected until Deinit()+Init().
  auto r2 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 100ms);
  EXPECT_EQ(r2.status, TransportStatus::kSessionDesynchronized);
  EXPECT_EQ(fake->send_calls, 1);  // the second call never reached the shim.
}

TEST(DiagTransport, ReinitAfterCrcMismatchAllowsFreshCommand) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> raw = {0x4B, 0x13, 0x0F, 0x00, 0x00, 0x00, 0x7e};
  fake->on_send = [&] { Fire(fake, raw); };
  auto r = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kFrameIntegrityFailed);

  t.Deinit();
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> payload = {0x4B, 0x13, 0x0F, 0x00, 9};
  fake->on_send = [&] { Fire(fake, FramedResponse(payload)); };
  auto r2 = t.SendSubsysCommand(0, {0x4B, 0x13, 0x0F, 0x00}, 500ms);
  EXPECT_EQ(r2.status, TransportStatus::kOk);
  EXPECT_EQ(r2.response, payload);
}

TEST(DiagTransport, RealReadDirEscapedCrcFrameNormalizesToOk) {
  // The real READDIR (seq=3) response whose CRC byte needed HDLC
  // unescaping (see diag_frame_test.cpp's H41RealReadDir* fixtures) -
  // exercised here at the SendSubsysCommand() level, not just
  // NormalizeDiagFrame() directly, confirming the fix is correctly
  // wired into the real request/response round trip.
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  FakeDiagCallbackShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  std::vector<uint8_t> raw = {
      0x4b, 0x13, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xff, 0xe1,
      0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x63, 0x66, 0x67,
      0x5f, 0x64, 0x65, 0x66, 0x5f, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67,
      0x5f, 0x68, 0x77, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e,
      0x00, 0x7d, 0x5d, 0x35, 0x7e,
  };
  ASSERT_EQ(raw.size(), 71u);
  fake->on_send = [&] { Fire(fake, raw); };

  auto r = t.SendSubsysCommand(0, {0x4b, 0x13, 0x0c, 0x00}, 500ms);
  ASSERT_EQ(r.status, TransportStatus::kOk);
  EXPECT_TRUE(r.crc_valid);
  EXPECT_TRUE(r.escape_present);
  EXPECT_EQ(r.escape_count, 1);
  EXPECT_EQ(r.response.size(), 67u);

  auto rd = ParseReadDirResponse(r.response.data(), r.response.size());
  ASSERT_TRUE(rd.ok);
  EXPECT_EQ(rd.dirp, 1u);
  EXPECT_EQ(rd.seqno, 3);
  EXPECT_EQ(rd.name, "mcfg_def_config_hw_version");
}

TEST(DiagTransport, ProductionDefaultRejectsUnframedReplyAndPoisonsSession) {
  auto owned = std::make_unique<FakeDiagCallbackShim>();
  auto* fake = owned.get();
  DiagTransport transport(std::move(owned)); // strict H.41 default
  std::string error;
  ASSERT_TRUE(transport.Init(&error));
  fake->on_send = [&] { Fire(fake, {0x4b, 0x13, 0x0f, 0x00}); };
  auto reply = transport.SendSubsysCommand(0, {0x4b, 0x13, 0x0f, 0x00}, 100ms);
  EXPECT_EQ(reply.status, TransportStatus::kFrameIntegrityFailed);
  EXPECT_EQ(transport.SendSubsysCommand(0, {0x4b, 0x13, 0x0f, 0x00}, 100ms).status,
      TransportStatus::kSessionDesynchronized);
  EXPECT_EQ(fake->send_calls, 1);
}

namespace {
// All cross-thread state here is atomic or protected by the gate mutex.
class BlockingShim : public IDiagCallbackShim {
 public:
  explicit BlockingShim(bool block_register) : block_register_(block_register) {}
  std::mutex gate;
  std::condition_variable gate_cv;
  bool entered = false, release = false;
  std::atomic<bool> in_shim{false}, overlap{false};
  std::atomic<int> deinit_calls{0};
  bool Init(std::string*) override { return true; }
  void Deinit() override {
    if (in_shim.load()) overlap.store(true);
    ++deinit_calls;
  }
  void Block() {
    in_shim.store(true);
    std::unique_lock<std::mutex> lock(gate);
    entered = true; gate_cv.notify_all();
    gate_cv.wait(lock, [&] { return release; });
    in_shim.store(false);
  }
  bool RegisterCallback(DiagDataCallback, void*, std::string*) override {
    if (block_register_) Block();
    return true;
  }
  bool SwitchLogging(int, std::string*) override { return true; }
  bool SendData(int, const void*, int, std::string*) override {
    if (!block_register_) Block();
    return true;
  }
 private:
  bool block_register_;
};
void CheckShutdownOrdering(bool block_register) {
  auto owned = std::make_unique<BlockingShim>(block_register);
  auto* fake = owned.get();
  DiagTransport transport(std::move(owned));
  std::string error;
  ASSERT_TRUE(transport.Init(&error));
  TransportStatus result = TransportStatus::kInternalStateError;
  std::thread sender([&] {
    result = transport.SendSubsysCommand(0, {0x4b, 0x13, 0x0f, 0}, 2s).status;
  });
  {
    std::unique_lock<std::mutex> lock(fake->gate);
    fake->gate_cv.wait(lock, [&] { return fake->entered; });
  }
  std::atomic<bool> shutdown_started{false};
  std::thread shutdown([&] { shutdown_started.store(true); transport.Deinit(); });
  while (!shutdown_started.load()) std::this_thread::yield();
  {
    std::unique_lock<std::mutex> lock(fake->gate);
    bool entered_early = fake->gate_cv.wait_for(lock, 20ms,
        [&] { return fake->deinit_calls.load() != 0; });
    EXPECT_FALSE(entered_early);
    fake->release = true;
    fake->gate_cv.notify_all();
  }
  sender.join(); shutdown.join();
  EXPECT_FALSE(fake->overlap.load());
  EXPECT_EQ(fake->deinit_calls.load(), 1);
  EXPECT_EQ(result, TransportStatus::kCancelled);
}
}  // namespace
TEST(DiagTransport, ShutdownWaitsForSynchronousSendBeforeDeinit) {
  CheckShutdownOrdering(false);
}
TEST(DiagTransport, ShutdownWaitsForRegistrationBeforeDeinit) {
  CheckShutdownOrdering(true);
}
