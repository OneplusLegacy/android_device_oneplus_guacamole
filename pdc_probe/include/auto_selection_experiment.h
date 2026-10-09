#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace guacamole_pdc {
// PDC 1.9 slot_id, cross-checked with H.41 request descriptor offset 0.
enum class FeatureSlot : uint32_t { kSlot0 = 0, kSlot1 = 1 };
enum class SelectionMode : uint32_t { kDisable = 0, kIin = 1, kImsi = 2, kHybrid = 3 };
enum class SelectionCarrier : uint32_t { kLgu = 38, kSkt = 39, kTta = 40 };
enum class RefreshMode : uint32_t { kDisabled = 0, kEnabled = 1 };
enum class AutoFeatureState { kEnabled, kDisabled, kUnknown };
// Compatibility for existing codec users; now explicitly slot IDs.
enum class ObservedAutoSelector { kZero, kOne };
std::vector<uint8_t> BuildAutoFeatureRequest(FeatureSlot slot, bool enable);
AutoFeatureState ClassifyAutoFeature(std::optional<uint32_t> mode,
                                    std::optional<bool> flex);
inline constexpr char kAutoFeatureProtocol[] = "AUTO_FEATURE_PROTOCOL_CONFIRMED";
inline constexpr char kAutoOriginalRestoreEffect[] = "AUTO_FEATURE_PERSISTENT_PRE_SKT_EFS_NOT_RESTORED";
inline constexpr uint16_t kObservedAutoMessage = 0x2e;
// Generic and B.4 device slot0 commands are CLOSED; historical codec tests remain.
inline constexpr bool kAutoExperimentSendOpen = false;
inline constexpr char kAutoExperimentConfirmation[] = "GUACAMOLE_AUTO_RESTORE_EXPERIMENT_V1";
// Pure host-side codec: exact subset emitted by the stock enable helper.
// It is not wired to QCCI. No arbitrary feature ID or raw request input.
std::vector<uint8_t> BuildObservedAutoEnableRequest(ObservedAutoSelector selector);
struct AutoSyncResult { bool valid=false; uint16_t result=0, error=0; };
struct AutoIndicationResult {
  bool valid=false;
  uint16_t error=0;
  std::optional<uint32_t> token;
};
AutoSyncResult ParseObservedAutoSync(const std::vector<uint8_t>& bytes);
AutoIndicationResult ParseObservedAutoIndication(const std::vector<uint8_t>& bytes);
bool IsClosedAutoExperimentCommand(const std::vector<std::string>& args);
}
