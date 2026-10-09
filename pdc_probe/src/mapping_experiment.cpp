#include "mapping_experiment.h"
#include "carrier_catalog.h"

namespace guacamole_pdc {
namespace {
constexpr uint32_t kTimeoutMs = 5000;
const auto kCommon=guacamole_catalog::Id(0);
const auto kSkt=guacamole_catalog::Id(1);
bool GoodSelected(const GetSelectedConfigOutcome& o) {
  return o.status == ProbeStatus::kOk && o.data.ok && o.data.error_status == 0;
}
bool Is(const std::optional<std::vector<uint8_t>>& id, MappingTarget target) {
  return id && *id == MappingId(target);
}
bool Known(const std::optional<std::vector<uint8_t>>& id) {
  return Is(id, MappingTarget::kCommon) || Is(id, MappingTarget::kSkt);
}
bool CommonIdle(const GetSelectedConfigOutcome& s) {
  return GoodSelected(s) && Is(s.data.active_id, MappingTarget::kCommon) && !s.data.pending_id;
}
}
const std::vector<uint8_t>& MappingId(MappingTarget target) {
  switch (target) {
    case MappingTarget::kCommon: return kCommon;
    case MappingTarget::kSkt: return kSkt;
  }
  static const std::vector<uint8_t> invalid;
  return invalid;
}
bool MappingInfoMatches(MappingTarget target, const GetConfigInfoOutcome& o) {
  if (o.status != ProbeStatus::kOk || !o.data.ok || o.data.error_status != 0 ||
      !o.data.has_total_size || !o.data.has_description || !o.data.has_version) return false;
  const int i=target==MappingTarget::kCommon?0:target==MappingTarget::kSkt?1:-1;
  return guacamole_catalog::Metadata(i,o.data.total_size,o.data.version,o.data.description);
}
std::optional<MappingRequest> ParseMappingArguments(const std::vector<std::string>& args) {
  if (args == std::vector<std::string>{"preflight"}) return MappingRequest{};
  if (args.size() != 3 || args[1] != "--confirm") return std::nullopt;
  if (args[0] == "select-skt-once" && args[2] == kSktConfirmation)
    return MappingRequest{MappingCommand::kSelectSkt, args[2]};
  if (args[0] == "select-common-once" && args[2] == kCommonConfirmation)
    return MappingRequest{MappingCommand::kSelectCommon, args[2]};
  return std::nullopt;
}
GetSelectedConfigOutcome MappingClientAdapter::Selected() {
  return client_.GetSelectedConfig(ConfigType::kSoftware, kTimeoutMs);
}
GetConfigInfoOutcome MappingClientAdapter::Info(MappingTarget target) {
  if (MappingId(target).empty()) return {};
  return client_.GetConfigInfo(ConfigType::kSoftware, MappingId(target), kTimeoutMs);
}
SetSelectedConfigOutcome MappingClientAdapter::Select(MappingTarget target) {
  if (MappingId(target).empty()) return {};
  return client_.SetSelectedConfig(ConfigType::kSoftware, MappingId(target), kTimeoutMs);
}
MappingReport RunMappingExperiment(IMappingClient& client, const MappingRequest& req,
    const std::function<void(const MappingReport&)>& before_selection) {
  MappingReport r;
  const bool recovery = req.command == MappingCommand::kSelectCommon;
  r.outcome = recovery ? "RECOVERY_BLOCKED" : "EXPERIMENT_BLOCKED";
  const bool preflight = req.command == MappingCommand::kPreflight;
  if ((preflight && !req.confirmation.empty()) ||
      (req.command == MappingCommand::kSelectSkt && req.confirmation != kSktConfirmation) ||
      (recovery && req.confirmation != kCommonConfirmation) ||
      (!preflight && !recovery && req.command != MappingCommand::kSelectSkt)) {
    r.reason = "invalid command/confirmation; no QMI calls";
    return r;
  }
  r.pre = client.Selected();
  r.common = client.Info(MappingTarget::kCommon);
  r.skt = client.Info(MappingTarget::kSkt);
  r.preflight_observed = true;
  if (!GoodSelected(r.pre) || !MappingInfoMatches(MappingTarget::kCommon, r.common) ||
      !MappingInfoMatches(MappingTarget::kSkt, r.skt)) {
    r.reason = "selected/info error or exact metadata mismatch";
    return r;
  }
  if (recovery) {
    // Require a known active ID; NONE/empty active is not an assumed recovery state.
    if (!Known(r.pre.data.active_id) ||
        (r.pre.data.pending_id && !Known(r.pre.data.pending_id))) {
      r.reason = "unknown/absent active or unknown pending ID";
      return r;
    }
    if (CommonIdle(r.pre)) { r.outcome = "NOOP_OK"; return r; }
  } else if (!CommonIdle(r.pre)) {
    r.reason = "requires exact Common active and pending NONE";
    return r;
  }
  if (preflight) { r.outcome = "PREFLIGHT_ALLOWED"; return r; }
  const auto target = recovery ? MappingTarget::kCommon : MappingTarget::kSkt;
  if (before_selection) before_selection(r);
  r.selection_attempted = true;
  r.selection = client.Select(target);  // exactly one; no retry/recovery branch
  r.post = client.Selected();           // observation only, including on ambiguous result
  r.post_observed = true;
  r.outcome = "USER_REVIEW_REQUIRED";
  if (r.selection.status != ProbeStatus::kOk || !r.selection.data.ok ||
      r.selection.data.error_status != 0) {
    r.reason = "SetSelected not confirmed; do not retry or auto recover";
    return r;
  }
  if (!GoodSelected(r.post)) { r.reason = "post GetSelected failed; state unknown"; return r; }
  const bool observed = (Is(r.post.data.active_id, target) && !r.post.data.pending_id) ||
      (Known(r.post.data.active_id) && Is(r.post.data.pending_id, target));
  if (!observed) { r.reason = "successful indication but unexpected post-state; STOP"; return r; }
  r.outcome = "TARGET_OBSERVED_REVIEW_REQUIRED";
  r.reason = "target active or pending observed; no activation/persistence/mapping proof";
  return r;
}
}
