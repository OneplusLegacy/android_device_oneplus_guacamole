#pragma once
#include <array>
#include <functional>
#include "known_profiles.h"
#include "mapping_experiment.h"

namespace guacamole_pdc {
enum class CarrierTarget { kCommon, kSkt, kKt, kLgu };
enum class CarrierOperation { kInspect, kInspectAll, kSelected, kLoad, kSelect, kH41Load };
struct CarrierRequest {
  CarrierOperation operation = CarrierOperation::kInspectAll;
  CarrierTarget target = CarrierTarget::kCommon;
  std::string confirmation;
};
const char* CarrierName(CarrierTarget target);
const std::vector<uint8_t>& CarrierId(CarrierTarget target);
const KnownProfile* CarrierFileProfile(CarrierTarget target);
std::string CarrierConfirmation(const CarrierRequest& request);
std::optional<CarrierRequest> ParseCarrierArguments(const std::vector<std::string>& args);
bool ValidCarrierRequest(const CarrierRequest& request);
ConfigPresence CarrierPresence(const GetConfigInfoOutcome& info);
bool CarrierInfoVerified(CarrierTarget target, const GetConfigInfoOutcome& info);
bool CarrierChunkAccepted(const LoadConfigChunkOutcome& outcome, size_t chunk_size,
                          size_t remaining);
struct CarrierFile {
  bool verified = false;
  std::vector<uint8_t> bytes;
  std::string error;
};
bool VerifyCarrierBytes(CarrierTarget target, const std::vector<uint8_t>& bytes);
// Fixed enum only; traverses every path component without following symlinks.
CarrierFile ReadCarrierFile(CarrierTarget target);

// No generic ID, raw path, Activate, Delete, reboot or EFS API.
class ICarrierClient {
 public:
  virtual ~ICarrierClient() = default;
  virtual GetSelectedConfigOutcome Selected() = 0;
  virtual GetConfigInfoOutcome Info(CarrierTarget target) = 0;
  virtual ListConfigsOutcome List() { return {}; }
  virtual GetConfigInfoOutcome SyntheticInfo() { return {}; }
  virtual LoadConfigChunkOutcome Chunk(CarrierTarget target, size_t offset) = 0;
  virtual SetSelectedConfigOutcome Select(CarrierTarget target) = 0;
};
struct CarrierReport {
  std::string outcome = "EXPERIMENT_BLOCKED";
  std::string reason;
  std::string load_gate = "NOT_EVALUATED";
  bool list_observed = false, synthetic_observed = false;
  ListConfigsOutcome list;
  GetConfigInfoOutcome synthetic;
  std::array<GetConfigInfoOutcome, 4> info;
  std::array<bool, 4> queried{};
  bool pre_observed = false, post_observed = false;
  GetSelectedConfigOutcome pre, post;
  bool mutation_attempted = false;
  size_t chunks_attempted = 0;
  bool transfer_completed = false;
  bool load_post_verified = false;
  LoadConfigChunkOutcome last_chunk;
  SetSelectedConfigOutcome selection;
  bool post_info_observed = false;
  GetConfigInfoOutcome post_info;
};
// Experimental eligibility only; NEVER changes ConfigPresence. File verification
// is an injected reader boundary; real transport independently hashes all bytes.
bool H41LoadCandidate(CarrierTarget target, const CarrierReport&, const CarrierFile&);

// Verified file is a file-reader boundary (fakeable in tests). Real main binds
// ReadCarrierFile and the transport independently revalidates before loading.
CarrierReport RunCarrierExperiment(ICarrierClient& client, const CarrierRequest& request,
    const CarrierFile& file,
    const std::function<void(const CarrierReport&)>& before_mutation = {});
}
