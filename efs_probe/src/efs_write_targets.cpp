#include "efs_write_targets.h"

#include <array>

namespace guacamole_efs {

namespace {
struct WriteTargetEntry {
  EfsWriteTarget target;
  const char* cli_name;
};

constexpr std::array<WriteTargetEntry, 1> kWriteTargets = {{
    {EfsWriteTarget::kMcfgSelDb, "sel-db"},
}};
}  // namespace

const std::string& TargetPath(EfsWriteTarget target) {
  switch (target) {
    case EfsWriteTarget::kMcfgSelDb: {
      static const std::string s = "/nv/item_files/mcfg/mcfg_sel_db.xml";
      return s;
    }
  }
  static const std::string kEmpty;
  return kEmpty;
}

std::optional<EfsWriteTarget> ResolveWriteTargetName(const std::string& name) {
  for (const auto& entry : kWriteTargets) {
    if (name == entry.cli_name) {
      return entry.target;
    }
  }
  return std::nullopt;
}

EfsTarget CorrespondingReadTarget(EfsWriteTarget target) {
  switch (target) {
    case EfsWriteTarget::kMcfgSelDb:
      return EfsTarget::kMcfgSelDb;
  }
  // Unreachable given the switch above is exhaustive over EfsWriteTarget's
  // single member today - no silent fallback value; a second write
  // target added without updating this function would fail to compile
  // (missing switch case, -Werror) rather than resolve here.
  return EfsTarget::kMcfgSelDb;
}

}  // namespace guacamole_efs
