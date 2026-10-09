#include "efs_targets.h"

#include <array>
#include <utility>

namespace guacamole_efs {

namespace {
struct TargetEntry {
  EfsTarget target;
  const char* cli_name;
};

// CLI name <-> enum mapping only - see TargetPath() below for the
// enum <-> path mapping (kept separate so each target's path has
// exactly one static definition, not duplicated here).
constexpr std::array<TargetEntry, 13> kTargets = {{
    {EfsTarget::kMcfgDir, "mcfg-dir"},
    {EfsTarget::kMcfgSelDb, "sel-db"},
    {EfsTarget::kMcfgAutoselectByUim, "autoselect"},
    {EfsTarget::kMcfgRfsSwDigestVersion, "rfs-digest"},
    {EfsTarget::kMcfgCards, "cards"},
    {EfsTarget::kMcfgDebugLog, "debug-log"},
    {EfsTarget::kMcfgDebugOldLog, "debug-old-log"},
    {EfsTarget::kEnableMcfgEfsLogging, "efslogging-flag"},
    {EfsTarget::kMcfgAutoselectByUimSubscription01, "autoselect-sub01"},
    {EfsTarget::kMcfgSetting, "setting"},
    {EfsTarget::kMcfgSetting1, "setting-1"},
    {EfsTarget::kMcfgSettingSubscription01, "setting-sub01"},
    {EfsTarget::kMcfgSettingSubscription01_1, "setting-sub01-1"},
}};
}  // namespace

const std::string& TargetPath(EfsTarget target) {
  // One real, permanent std::string per case - no shared/reused storage
  // across different targets (a single reused static, keyed only by
  // call order, would let an earlier caller's reference silently start
  // pointing at a different target's path after a later call).
  switch (target) {
    case EfsTarget::kMcfgDir: {
      static const std::string s = "/nv/item_files/mcfg";
      return s;
    }
    case EfsTarget::kMcfgSelDb: {
      static const std::string s = "/nv/item_files/mcfg/mcfg_sel_db.xml";
      return s;
    }
    case EfsTarget::kMcfgAutoselectByUim: {
      static const std::string s =
          "/nv/item_files/mcfg/mcfg_autoselect_by_uim";
      return s;
    }
    case EfsTarget::kMcfgRfsSwDigestVersion: {
      static const std::string s =
          "/nv/item_files/mcfg/mcfg_rfs_sw_digest_version";
      return s;
    }
    case EfsTarget::kMcfgCards: {
      static const std::string s = "/nv/item_files/mcfg/cards";
      return s;
    }
    case EfsTarget::kMcfgDebugLog: {
      static const std::string s = "/mcfg_debug.log";
      return s;
    }
    case EfsTarget::kMcfgDebugOldLog: {
      static const std::string s = "/mcfg_debug_old.log";
      return s;
    }
    case EfsTarget::kEnableMcfgEfsLogging: {
      static const std::string s = "/enable_mcfg_efslogging";
      return s;
    }
    case EfsTarget::kMcfgAutoselectByUimSubscription01: {
      static const std::string s =
          "/nv/item_files/mcfg/mcfg_autoselect_by_uim_Subscription01";
      return s;
    }
    case EfsTarget::kMcfgSetting: {
      static const std::string s = "/nv/item_files/mcfg/mcfg_setting";
      return s;
    }
    case EfsTarget::kMcfgSetting1: {
      static const std::string s = "/nv/item_files/mcfg/mcfg_setting_1";
      return s;
    }
    case EfsTarget::kMcfgSettingSubscription01: {
      static const std::string s =
          "/nv/item_files/mcfg/mcfg_setting_Subscription01";
      return s;
    }
    case EfsTarget::kMcfgSettingSubscription01_1: {
      static const std::string s =
          "/nv/item_files/mcfg/mcfg_setting_Subscription01_1";
      return s;
    }
  }
  static const std::string kEmpty;
  return kEmpty;
}

std::optional<EfsTarget> ResolveTargetName(const std::string& name) {
  for (const auto& entry : kTargets) {
    if (name == entry.cli_name) {
      return entry.target;
    }
  }
  return std::nullopt;
}

}  // namespace guacamole_efs
