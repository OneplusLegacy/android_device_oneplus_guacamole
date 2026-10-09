#include "efs_sessions.h"
#include <gtest/gtest.h>
#include <vector>
#include <string>

using namespace guacamole_efs;
namespace {
struct State { std::vector<std::string> events; bool init_ok = true; int sends = 0; };
class Shim : public IDiagCallbackShim {
 public:
  explicit Shim(State* state) : state_(state) {}
  bool Init(std::string*) override { state_->events.push_back("init"); return state_->init_ok; }
  void Deinit() override { state_->events.push_back("deinit"); }
  bool RegisterCallback(DiagDataCallback cb, void* context, std::string*) override {
    cb_ = cb; context_ = context; return true;
  }
  bool SwitchLogging(int, std::string*) override { return true; }
  bool SendData(int, const void*, int, std::string*) override {
    ++state_->sends;
    // Correct STAT header but no HDLC trailer/CRC: strict factory must reject.
    std::vector<uint8_t> raw(32, 0);
    raw[0] = 0x4b; raw[1] = 0x13; raw[2] = 0x0f;
    cb_(raw.data(), static_cast<int>(raw.size()), context_);
    return true;
  }
 private:
  State* state_;
  DiagDataCallback cb_ = nullptr;
  void* context_ = nullptr;
};
}
TEST(DiagEfsSessions, EachOwnedPhaseDeinitializesBeforeNextInit) {
  State state;
  DiagEfsSessionFactory factory([&] { return std::make_unique<Shim>(&state); });
  std::string error;
  { auto s = factory.OpenRead(&error); ASSERT_NE(s, nullptr); }
  { auto s = factory.OpenWrite(&error); ASSERT_NE(s, nullptr); }
  { auto s = factory.OpenRead(&error); ASSERT_NE(s, nullptr); }
  EXPECT_EQ(state.events, (std::vector<std::string>{"init", "deinit", "init", "deinit", "init", "deinit"}));
  EXPECT_EQ(state.sends, 0); // Merely opening an owned session does not mutate EFS.
}
TEST(DiagEfsSessions, TwoFactoriesCannotOwnOverlappingSessions) {
  State state;
  DiagEfsSessionFactory first([&] { return std::make_unique<Shim>(&state); });
  DiagEfsSessionFactory second([&] { return std::make_unique<Shim>(&state); });
  std::string error;
  auto read = first.OpenRead(&error);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(second.OpenWrite(&error), nullptr);
  EXPECT_EQ(state.events.size(), 1u);
  read.reset();
  EXPECT_NE(second.OpenWrite(&error), nullptr);
}
TEST(DiagEfsSessions, StrictFactoryRejectsRawAndReleasesLeaseAfterFailure) {
  State state;
  DiagEfsSessionFactory factory([&] { return std::make_unique<Shim>(&state); });
  std::string error;
  auto read = factory.OpenRead(&error);
  ASSERT_NE(read, nullptr);
  EXPECT_FALSE(read->client().Stat("/nv/item_files/mcfg/mcfg_sel_db.xml").ok);
  read.reset();
  EXPECT_NE(factory.OpenRead(&error), nullptr);
}
TEST(DiagEfsSessions, InitFailureReleasesOwnedSessionGate) {
  State state; state.init_ok = false;
  DiagEfsSessionFactory factory([&] { return std::make_unique<Shim>(&state); });
  std::string error;
  EXPECT_EQ(factory.OpenRead(&error), nullptr);
  state.init_ok = true;
  EXPECT_NE(factory.OpenRead(&error), nullptr);
}
