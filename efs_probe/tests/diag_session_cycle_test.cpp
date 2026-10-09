#include "diag_session_cycle.h"
#include "diag_callback_shim.h"
#include "diag_frame.h"
#include "efs_targets.h"
#include "hash.h"
#include <gtest/gtest.h>

using namespace guacamole_efs;
namespace {
struct Trace {
  int made = 0;
  int init_fail_at = 0;
  int fail_read_at = 0;
  bool timeout = false;
  bool raw = false;
  bool corrupt = false;
  bool mismatch = false;
  std::vector<std::string> events;
  std::vector<uint16_t> commands;
};
class CycleShim : public IDiagCallbackShim {
 public:
  explicit CycleShim(Trace& trace) : trace_(trace), id_(++trace.made) {}
  bool Init(std::string*) override {
    trace_.events.push_back("init" + std::to_string(id_));
    return id_ != trace_.init_fail_at;
  }
  void Deinit() override { trace_.events.push_back("deinit" + std::to_string(id_)); }
  bool RegisterCallback(DiagDataCallback cb, void* context, std::string*) override {
    cb_ = cb; context_ = context; return true;
  }
  bool SwitchLogging(int mode, std::string*) override { return mode == kDiagCallbackMode; }
  bool SendData(int, const void* bytes, int len, std::string*) override {
    if (len < 4) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    const uint16_t cmd = uint16_t(p[2]) | (uint16_t(p[3]) << 8);
    trace_.commands.push_back(cmd);
    std::vector<uint8_t> r{0x4b, 0x13, p[2], p[3]};
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) r.push_back(v >> (8 * i)); };
    const auto& path = TargetPath(EfsTarget::kMcfgRfsSwDigestVersion);
    switch (cmd) {
      case kEfsCmdStat:
        EXPECT_EQ(std::vector<uint8_t>(p, p + len), BuildStatRequest(path));
        u32(0); u32(0100644); u32(32); u32(1); u32(0); u32(0); u32(0); break;
      case kEfsCmdOpen:
        EXPECT_EQ(std::vector<uint8_t>(p, p + len), BuildOpenReadOnlyRequest(path, 0));
        u32(7); u32(0); break;
      case kEfsCmdRead:
        EXPECT_EQ(std::vector<uint8_t>(p, p + len), BuildReadRequest(7, 32, 0));
        trace_.events.push_back("read" + std::to_string(id_));
        if (id_ == trace_.fail_read_at && trace_.timeout) return true;
        u32(7); u32(0); u32(32); u32(0);
        r.insert(r.end(), 32, trace_.mismatch && id_ == 2 ? 2 : 1); break;
      case kEfsCmdClose: u32(0); break;
      default: ADD_FAILURE() << "mutating/unexpected command " << cmd; return false;
    }
    if (cmd == kEfsCmdRead && id_ == trace_.fail_read_at && trace_.raw) {
      cb_(r.data(), r.size(), context_); return true;
    }
    uint16_t crc = Crc16X25(r.data(), r.size());
    if (cmd == kEfsCmdRead && id_ == trace_.fail_read_at && trace_.corrupt) crc ^= 1;
    r.push_back(crc & 255); r.push_back(crc >> 8);
    std::vector<uint8_t> framed;
    for (uint8_t b : r) {
      if (b == 0x7e || b == 0x7d) { framed.push_back(0x7d); framed.push_back(b ^ 0x20); }
      else framed.push_back(b);
    }
    framed.push_back(0x7e); cb_(framed.data(), framed.size(), context_); return true;
  }
 private:
  Trace& trace_;
  int id_;
  DiagDataCallback cb_ = nullptr;
  void* context_ = nullptr;
};
}
TEST(DiagSessionCycle, StrictOwnedTeardownBeforeSecondInitAndExactEquality) {
  Trace t; DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_TRUE(r.exact_equal);
  EXPECT_EQ(t.events, (std::vector<std::string>{"init1", "read1", "deinit1", "init2", "read2", "deinit2"}));
  for (const auto& p : r.sessions) {
    EXPECT_EQ(p.status, "READ_OK"); EXPECT_TRUE(p.teardown_completed);
    EXPECT_EQ(p.bytes, std::vector<uint8_t>(32, 1));
    EXPECT_EQ(p.sha256, Sha256(p.bytes.data(), p.bytes.size()));
  }
  EXPECT_EQ(t.commands, (std::vector<uint16_t>{15,2,4,3,15,2,4,3}));
}
TEST(DiagSessionCycle, FirstInitFailurePreventsSecondSession) {
  Trace t; t.init_fail_at = 1;
  DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_EQ(r.sessions[0].status, "INIT_FAILED");
  EXPECT_EQ(r.sessions[1].status, "NOT_STARTED"); EXPECT_EQ(t.made, 1);
  EXPECT_TRUE(t.commands.empty()); EXPECT_FALSE(r.exact_equal);
}
TEST(DiagSessionCycle, TimeoutPoisonsNoRetryAndNoSecondSession) {
  Trace t; t.fail_read_at = 1; t.timeout = true;
  DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); }, std::chrono::milliseconds(1));
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_EQ(t.made, 1); EXPECT_EQ(t.commands, (std::vector<uint16_t>{15,2,4}));
  EXPECT_EQ(r.sessions[0].status, "READ_FAILED"); EXPECT_TRUE(r.sessions[0].teardown_completed);
  EXPECT_EQ(r.sessions[1].status, "NOT_STARTED"); EXPECT_FALSE(r.exact_equal);
}
TEST(DiagSessionCycle, StrictRawOrBadCrcFailsAndStops) {
  for (bool raw : {true, false}) {
    Trace t; t.fail_read_at = 1; t.raw = raw; t.corrupt = !raw;
    DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
    const auto r = RunDiagSessionCycleReadonly(factory);
    EXPECT_EQ(t.made, 1); EXPECT_EQ(t.commands, (std::vector<uint16_t>{15,2,4}));
    EXPECT_EQ(r.sessions[0].status, "READ_FAILED"); EXPECT_TRUE(r.sessions[0].teardown_completed);
    EXPECT_FALSE(r.exact_equal);
  }
}
TEST(DiagSessionCycle, OverlappingOwnerPreventsEvenFirstInit) {
  Trace t; DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  std::string error; auto held = factory.OpenRead(&error); ASSERT_NE(held, nullptr);
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_EQ(t.made, 1); EXPECT_EQ(r.sessions[0].status, "INIT_FAILED");
  EXPECT_EQ(r.sessions[1].status, "NOT_STARTED");
}
TEST(DiagSessionCycle, MismatchFailsDespiteBothSuccessfulReads) {
  Trace t; t.mismatch = true;
  DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_FALSE(r.exact_equal); EXPECT_NE(r.sessions[0].sha256, r.sessions[1].sha256);
  EXPECT_TRUE(r.sessions[0].teardown_completed); EXPECT_TRUE(r.sessions[1].teardown_completed);
}
TEST(DiagSessionCycle, SecondInitFailurePreservesFirstResult) {
  Trace t; t.init_fail_at = 2;
  DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  const auto r = RunDiagSessionCycleReadonly(factory);
  EXPECT_EQ(r.sessions[0].status, "READ_OK"); EXPECT_TRUE(r.sessions[0].teardown_completed);
  EXPECT_EQ(r.sessions[1].status, "INIT_FAILED"); EXPECT_FALSE(r.exact_equal);
}

TEST(DiagSessionCycle, SingleWorkerReadUsesOnlyFixedReadonlyCommands) {
  Trace t;
  DiagEfsSessionFactory factory([&] { return std::make_unique<CycleShim>(t); });
  const auto r = RunSingleDiagDigestRead(factory);
  EXPECT_EQ(r.status, "READ_OK");
  EXPECT_TRUE(r.teardown_completed);
  EXPECT_EQ(t.made, 1);
  EXPECT_EQ(t.events, (std::vector<std::string>{"init1", "read1", "deinit1"}));
  EXPECT_EQ(t.commands, (std::vector<uint16_t>{15,2,4,3}));
  EXPECT_EQ(r.bytes, std::vector<uint8_t>(32, 1));
  EXPECT_EQ(r.sha256, Sha256(r.bytes.data(), r.bytes.size()));
}
