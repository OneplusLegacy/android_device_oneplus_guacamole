#include "stat_mcfg.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "diag_callback_shim.h"
#include "efs_targets.h"

using namespace guacamole_efs;
using namespace std::chrono_literals;

namespace {

class FakeShim : public IDiagCallbackShim {
 public:
  DiagDataCallback captured_callback = nullptr;
  void* captured_context = nullptr;
  int send_calls = 0;
  std::function<void()> on_send;

  bool Init(std::string*) override { return true; }
  void Deinit() override {}
  bool RegisterCallback(DiagDataCallback cb, void* context,
                         std::string*) override {
    captured_callback = cb;
    captured_context = context;
    return true;
  }
  bool SwitchLogging(int, std::string*) override { return true; }
  bool SendData(int, const void*, int, std::string*) override {
    send_calls++;
    if (on_send) on_send();
    return true;
  }
};

void Fire(FakeShim* shim, std::vector<uint8_t> data) {
  ASSERT_NE(shim->captured_callback, nullptr);
  shim->captured_callback(data.data(), static_cast<int>(data.size()),
                           shim->captured_context);
}

// A structurally-valid STAT response: the 4-byte echoed header plus 7
// little-endian int32 fields, matching efs_wire.h's confirmed layout.
std::vector<uint8_t> MakeStatResponse(int32_t diag_errno, int32_t mode,
                                       int32_t size, int32_t nlink,
                                       int32_t atime, int32_t mtime,
                                       int32_t ctime) {
  std::vector<uint8_t> resp = {0x4B, 0x13, 0x0F, 0x00};
  auto push_i32 = [&](int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    resp.push_back(u & 0xFF);
    resp.push_back((u >> 8) & 0xFF);
    resp.push_back((u >> 16) & 0xFF);
    resp.push_back((u >> 24) & 0xFF);
  };
  push_i32(diag_errno);
  push_i32(mode);
  push_i32(size);
  push_i32(nlink);
  push_i32(atime);
  push_i32(mtime);
  push_i32(ctime);
  return resp;
}

}  // namespace

TEST(StatMcfg, StatMcfgRequestStartsWith4B130F00) {
  auto owned = std::make_unique<FakeShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto result = RunStatMcfg(&t, 60ms);  // no response fired - only .request matters here.

  ASSERT_GE(result.request.size(), 4u);
  std::vector<uint8_t> expected_header = {0x4B, 0x13, 0x0F, 0x00};
  EXPECT_EQ(
      (std::vector<uint8_t>(result.request.begin(), result.request.begin() + 4)),
      expected_header);
}

TEST(StatMcfg, StatMcfgUsesExactWhitelistedPath) {
  auto owned = std::make_unique<FakeShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto result = RunStatMcfg(&t, 60ms);

  const std::string& expected_path = TargetPath(EfsTarget::kMcfgDir);
  ASSERT_EQ(expected_path, "/nv/item_files/mcfg");
  ASSERT_GE(result.request.size(), 4 + expected_path.size() + 1);
  std::string actual_path(
      reinterpret_cast<const char*>(result.request.data() + 4),
      expected_path.size());
  EXPECT_EQ(actual_path, expected_path);
  EXPECT_EQ(result.request[4 + expected_path.size()], 0x00);  // NUL terminator.
}

TEST(StatMcfg, StatMcfgNeverAcceptsCallerSuppliedPath) {
  // RunStatMcfg() has no path parameter at all - this is a compile-time
  // guarantee, and this test is the runtime corroboration: every call,
  // on every transport, sends byte-identical request bytes, since
  // there is no way to make it vary.
  auto owned1 = std::make_unique<FakeShim>();
  DiagTransport t1(std::move(owned1), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t1.Init(&error));
  auto r1 = RunStatMcfg(&t1, 60ms);

  auto owned2 = std::make_unique<FakeShim>();
  DiagTransport t2(std::move(owned2), DiagFrameMode::kTestAllowUnframed);
  ASSERT_TRUE(t2.Init(&error));
  auto r2 = RunStatMcfg(&t2, 60ms);

  EXPECT_EQ(r1.request, r2.request);
}

TEST(StatMcfg, StatMcfgSuccessfulResponseParsed) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    Fire(fake, MakeStatResponse(/*diag_errno=*/0, /*mode=*/0040755,
                                 /*size=*/4096, /*nlink=*/1, /*atime=*/111,
                                 /*mtime=*/222, /*ctime=*/333));
  };

  auto result = RunStatMcfg(&t, 500ms);
  ASSERT_EQ(result.transport.status, TransportStatus::kOk);
  ASSERT_TRUE(result.parsed);
  EXPECT_EQ(result.stat.diag_errno, 0);
  EXPECT_EQ(result.stat.size, 4096);
  EXPECT_EQ(result.stat.nlink, 1);
}

TEST(StatMcfg, StatMcfgNonzeroDiagErrnoFails) {
  // Transport success and EFS success are different things - a
  // response can arrive, be structurally well-formed, and still carry
  // a nonzero diag_errno (the modem's own EFS layer reporting an
  // error). Callers MUST check .stat.diag_errno, not just
  // .transport.status - this test documents and enforces that
  // distinction.
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  fake->on_send = [&] {
    Fire(fake, MakeStatResponse(/*diag_errno=*/2, 0, 0, 0, 0, 0, 0));
  };

  auto result = RunStatMcfg(&t, 500ms);
  ASSERT_EQ(result.transport.status, TransportStatus::kOk);
  ASSERT_TRUE(result.parsed);
  EXPECT_NE(result.stat.diag_errno, 0);
}

TEST(StatMcfg, StatMcfgWrongHeaderIgnoredThenTimesOut) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  // Wrong subsys/cmd - not a STAT response to this request at all.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x99, 0x01, 0x00}); };

  auto result = RunStatMcfg(&t, 80ms);
  EXPECT_EQ(result.transport.status, TransportStatus::kTimeout);
  EXPECT_FALSE(result.parsed);
}

TEST(StatMcfg, StatMcfgMalformedResponseFails) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  // Correct header (correlation matches), but far too short to be a
  // real 28-byte STAT payload.
  fake->on_send = [&] { Fire(fake, {0x4B, 0x13, 0x0F, 0x00, 1, 2, 3}); };

  auto result = RunStatMcfg(&t, 500ms);
  ASSERT_EQ(result.transport.status, TransportStatus::kOk);
  EXPECT_FALSE(result.parsed);
}

TEST(StatMcfg, StatMcfgTimeoutDoesNotRetry) {
  auto owned = std::make_unique<FakeShim>();
  FakeShim* fake = owned.get();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));
  // No response fired at all.

  auto result = RunStatMcfg(&t, 60ms);
  EXPECT_EQ(result.transport.status, TransportStatus::kTimeout);
  EXPECT_EQ(fake->send_calls, 1);  // exactly one attempt - no retry.
}

TEST(StatMcfg, StatMcfgDoesNotInvokeAnyMutatingCommand) {
  auto owned = std::make_unique<FakeShim>();
  DiagTransport t(std::move(owned), DiagFrameMode::kTestAllowUnframed);
  std::string error;
  ASSERT_TRUE(t.Init(&error));

  auto result = RunStatMcfg(&t, 60ms);
  ASSERT_GE(result.request.size(), 4u);
  // Command bytes are exactly kEfsCmdStat (15) - nowhere near any
  // mutating command code, by construction (efs_wire.h defines no
  // mutating command constant anywhere to accidentally reference).
  EXPECT_EQ(result.request[2], static_cast<uint8_t>(kEfsCmdStat & 0xFF));
  EXPECT_EQ(result.request[3],
            static_cast<uint8_t>((kEfsCmdStat >> 8) & 0xFF));
}
