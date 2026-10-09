#include "efs_targets.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "efs_client.h"

using namespace guacamole_efs;

namespace {
// Spy client: records whether ANY method was ever called, so a test can
// prove a rejected/unresolved target never reaches the transport layer
// at all - not just that it "returns an error after calling out".
class SpyEfsClient : public IEfsClient {
 public:
  int total_calls = 0;

  EfsStatResult Stat(const std::string&) override {
    total_calls++;
    return {};
  }
  EfsOpenResult OpenReadOnly(const std::string&) override {
    total_calls++;
    return {};
  }
  EfsReadResult Read(int32_t, uint32_t, uint32_t) override {
    total_calls++;
    return {};
  }
  EfsCloseResult Close(int32_t) override {
    total_calls++;
    return {};
  }
  EfsOpenDirResult OpenDir(const std::string&) override {
    total_calls++;
    return {};
  }
  EfsReadDirResult ReadDir(uint32_t, int32_t) override {
    total_calls++;
    return {};
  }
  EfsCloseDirResult CloseDir(uint32_t) override {
    total_calls++;
    return {};
  }
};

// The only "orchestration" a caller can do with a target name: resolve
// it, and only proceed to a (fake, here) client call if resolution
// succeeded. This mirrors exactly what a future CLI command handler
// must do - there is no other way to get from a string to a client
// call in this codebase.
int StatIfWhitelisted(SpyEfsClient* client, const std::string& name) {
  auto target = ResolveTargetName(name);
  if (!target.has_value()) {
    return -1;  // rejected before ever touching the client.
  }
  client->Stat(TargetPath(*target));
  return 0;
}
}  // namespace

// All 13 fixed target names - the original 8 plus the 5 added
// 2026-09-23 (second whitelist expansion) whose exact names came from
// the third real snapshot-mcfg-readonly attempt's real READDIR
// enumeration of /nv/item_files/mcfg - see efs_targets.h.
constexpr const char* kAllTargetNames[] = {
    "mcfg-dir",         "sel-db",          "autoselect",
    "rfs-digest",       "cards",           "debug-log",
    "debug-old-log",    "efslogging-flag", "autoselect-sub01",
    "setting",          "setting-1",       "setting-sub01",
    "setting-sub01-1",
};

TEST(EfsTargets, AllThirteenFixedTargetsResolve) {
  for (const char* name : kAllTargetNames) {
    auto t = ResolveTargetName(name);
    ASSERT_TRUE(t.has_value()) << name;
    EXPECT_FALSE(TargetPath(*t).empty()) << name;
  }
}

TEST(EfsTargets, DistinctTargetsHaveDistinctPaths) {
  std::vector<std::string> paths;
  for (const char* name : kAllTargetNames) {
    paths.push_back(TargetPath(*ResolveTargetName(name)));
  }
  for (size_t i = 0; i < paths.size(); i++) {
    for (size_t j = i + 1; j < paths.size(); j++) {
      EXPECT_NE(paths[i], paths[j]) << i << " vs " << j;
    }
  }
}

// --- 2026-09-23 second whitelist expansion: the 5 new targets, one
// test each verifying its exact real-device-confirmed path. ---

TEST(EfsTargets, AutoselectSubscription01ExactPath) {
  EXPECT_EQ(TargetPath(EfsTarget::kMcfgAutoselectByUimSubscription01),
            "/nv/item_files/mcfg/mcfg_autoselect_by_uim_Subscription01");
}

TEST(EfsTargets, SettingExactPath) {
  EXPECT_EQ(TargetPath(EfsTarget::kMcfgSetting),
            "/nv/item_files/mcfg/mcfg_setting");
}

TEST(EfsTargets, Setting1ExactPath) {
  EXPECT_EQ(TargetPath(EfsTarget::kMcfgSetting1),
            "/nv/item_files/mcfg/mcfg_setting_1");
}

TEST(EfsTargets, SettingSubscription01ExactPath) {
  EXPECT_EQ(TargetPath(EfsTarget::kMcfgSettingSubscription01),
            "/nv/item_files/mcfg/mcfg_setting_Subscription01");
}

TEST(EfsTargets, SettingSubscription01_1ExactPath) {
  EXPECT_EQ(TargetPath(EfsTarget::kMcfgSettingSubscription01_1),
            "/nv/item_files/mcfg/mcfg_setting_Subscription01_1");
}

TEST(EfsTargets, NonWhitelistedPathRejectedBeforeTransport) {
  SpyEfsClient client;
  int rc = StatIfWhitelisted(&client, "/etc/passwd");
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(client.total_calls, 0);
}

TEST(EfsTargets, ArbitraryTraversalPathRejectedBeforeTransport) {
  SpyEfsClient client;
  int rc = StatIfWhitelisted(&client, "../../../data/data/com.evil/x");
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(client.total_calls, 0);
}

TEST(EfsTargets, EmptyNameRejectedBeforeTransport) {
  SpyEfsClient client;
  int rc = StatIfWhitelisted(&client, "");
  EXPECT_EQ(rc, -1);
  EXPECT_EQ(client.total_calls, 0);
}

TEST(EfsTargets, CaseSensitiveNoAccidentalMatch) {
  // Guards against a whitelist that's accidentally case-insensitive and
  // therefore broader than intended.
  EXPECT_FALSE(ResolveTargetName("Sel-Db").has_value());
  EXPECT_FALSE(ResolveTargetName("SEL-DB").has_value());
}

TEST(EfsTargets, RawTargetPathStringNotAcceptedAsAName) {
  // The whitelist maps NAMES to paths, one-way - passing an already
  //-resolved path string back in as if it were a name must not
  // accidentally "resolve" (that would mean two inputs both work,
  // widening the effective whitelist surface unintentionally).
  EXPECT_FALSE(
      ResolveTargetName("/nv/item_files/mcfg/mcfg_sel_db.xml").has_value());
}
