#pragma once
#include <optional>
#include <functional>
#include <string>
#include <vector>
#include "pdc_client.h"

namespace guacamole_pdc {
enum class MappingTarget { kCommon, kSkt };
enum class MappingCommand { kPreflight, kSelectSkt, kSelectCommon };
inline constexpr char kSktConfirmation[] = "GUACAMOLE_SKT_SETSELECTED_EXPERIMENT_V1";
inline constexpr char kCommonConfirmation[] = "GUACAMOLE_COMMON_RECOVERY_EXPERIMENT_V1";
struct MappingRequest {
  MappingCommand command = MappingCommand::kPreflight;
  std::string confirmation;
};
std::optional<MappingRequest> ParseMappingArguments(const std::vector<std::string>& args);
const std::vector<uint8_t>& MappingId(MappingTarget target);
bool MappingInfoMatches(MappingTarget target, const GetConfigInfoOutcome& info);

// No arbitrary ID, Load, Activate, Delete, EFS or reboot operation in this API.
class IMappingClient {
 public:
  virtual ~IMappingClient() = default;
  virtual GetSelectedConfigOutcome Selected() = 0;
  virtual GetConfigInfoOutcome Info(MappingTarget target) = 0;
  virtual SetSelectedConfigOutcome Select(MappingTarget target) = 0;
};
class MappingClientAdapter final : public IMappingClient {
 public:
  explicit MappingClientAdapter(IPdcClient& client) : client_(client) {}
  GetSelectedConfigOutcome Selected() override;
  GetConfigInfoOutcome Info(MappingTarget target) override;
  SetSelectedConfigOutcome Select(MappingTarget target) override;
 private:
  IPdcClient& client_;
};
struct MappingReport {
  std::string outcome = "EXPERIMENT_BLOCKED";
  std::string reason;
  GetSelectedConfigOutcome pre;
  GetConfigInfoOutcome common;
  GetConfigInfoOutcome skt;
  bool preflight_observed = false;
  bool selection_attempted = false;
  SetSelectedConfigOutcome selection;
  bool post_observed = false;
  GetSelectedConfigOutcome post;
};
MappingReport RunMappingExperiment(IMappingClient& client, const MappingRequest& request,
    const std::function<void(const MappingReport&)>& before_selection = {});
}
