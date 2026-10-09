#include "carrier_experiment.h"
#include "hash.h"
#include "presence_diagnostics.h"
#include <algorithm>
#include "carrier_catalog.h"

namespace guacamole_pdc {
namespace {
constexpr std::array<CarrierTarget, 4> kTargets{CarrierTarget::kCommon, CarrierTarget::kSkt,
    CarrierTarget::kKt, CarrierTarget::kLgu};
size_t Index(CarrierTarget t) { return static_cast<size_t>(t); }
bool GoodSelected(const GetSelectedConfigOutcome& s) {
  return s.status == ProbeStatus::kOk && s.data.ok && s.data.error_status == 0 && s.data.error_code == 0;
}
bool CommonIdle(const GetSelectedConfigOutcome& s) {
  return GoodSelected(s) && s.data.active_id == CarrierId(CarrierTarget::kCommon) && !s.data.pending_id;
}
bool VerifiedId(const std::optional<std::vector<uint8_t>>& id, const CarrierReport& r) {
  if (!id || id->empty()) return false;
  for (auto t : kTargets)
    if (*id == CarrierId(t) && r.queried[Index(t)] && CarrierInfoVerified(t, r.info[Index(t)])) return true;
  return false;
}
}
const char* CarrierName(CarrierTarget t) {
  switch (t) {
    case CarrierTarget::kCommon: return "common";
    case CarrierTarget::kSkt: return "skt";
    case CarrierTarget::kKt: return "kt";
    case CarrierTarget::kLgu: return "lgu";
  }
  return "invalid";
}
const std::vector<uint8_t>& CarrierId(CarrierTarget t) {
 static const std::array<std::vector<uint8_t>,4> ids{guacamole_catalog::Id(0),guacamole_catalog::Id(1),guacamole_catalog::Id(2),guacamole_catalog::Id(3)};
 static const std::vector<uint8_t> invalid;
 int i=static_cast<int>(t);return guacamole_catalog::Find(i)?ids[i]:invalid;
}
const KnownProfile* CarrierFileProfile(CarrierTarget t) {
  switch (t) {
    case CarrierTarget::kSkt: return &GetSktProfile();
    case CarrierTarget::kKt: return &GetKtProfile();
    case CarrierTarget::kLgu: return &GetLguProfile();
    case CarrierTarget::kCommon: return nullptr;
  }
  return nullptr;
}
std::string CarrierConfirmation(const CarrierRequest& r) {
  const char* name = r.target == CarrierTarget::kKt ? "KT" : r.target == CarrierTarget::kLgu ? "LGU" : nullptr;
  if (r.operation == CarrierOperation::kSelect && r.target == CarrierTarget::kCommon) return kCommonConfirmation;
  if (!name) return {};
  if (r.operation == CarrierOperation::kH41Load) return std::string("GUACAMOLE_") + name + "_H41_LOAD_EXPERIMENT_V1";
  if (r.operation == CarrierOperation::kLoad) return std::string("GUACAMOLE_") + name + "_LOAD_EXPERIMENT_V1";
  if (r.operation == CarrierOperation::kSelect) return std::string("GUACAMOLE_") + name + "_SETSELECTED_EXPERIMENT_V1";
  return {};
}
bool ValidCarrierRequest(const CarrierRequest& r) {
  if (CarrierId(r.target).empty()) return false;
  switch (r.operation) {
    case CarrierOperation::kInspect:
    case CarrierOperation::kInspectAll:
    case CarrierOperation::kSelected: return r.confirmation.empty();
    case CarrierOperation::kH41Load:
    case CarrierOperation::kLoad:
    case CarrierOperation::kSelect: {
      const auto expected = CarrierConfirmation(r);
      return !expected.empty() && r.confirmation == expected;
    }
  }
  return false;
}
std::optional<CarrierRequest> ParseCarrierArguments(const std::vector<std::string>& a) {
  if (a == std::vector<std::string>{"inspect-all"}) return CarrierRequest{};
  if (a == std::vector<std::string>{"selected"}) return CarrierRequest{CarrierOperation::kSelected, CarrierTarget::kCommon, ""};
  for (auto t : kTargets) {
    if (a == std::vector<std::string>{std::string("inspect-") + CarrierName(t)})
      return CarrierRequest{CarrierOperation::kInspect, t, ""};
    if (a.size() != 3 || a[1] != "--confirm") continue;
    for (auto op : {CarrierOperation::kLoad, CarrierOperation::kSelect, CarrierOperation::kH41Load}) {
      const std::string command = std::string(op == CarrierOperation::kSelect ? "select-" : "load-") + CarrierName(t) + (op == CarrierOperation::kH41Load ? "-h41-once" : "-once");
      CarrierRequest r{op, t, a[2]};
      if (a[0] == command && ValidCarrierRequest(r)) return r;
    }
  }
  return std::nullopt;
}
ConfigPresence CarrierPresence(const GetConfigInfoOutcome& o) {
  if (o.status == ProbeStatus::kOk && o.data.ok && !o.data.error_status && !o.data.error_code)
    return ConfigPresence::kExists;
  // Preserve the established indication-only NO_ENTRY policy. The indication
  // contains ONE u16 result, stored as error_status, NOT sync error_code.
  if (o.status == ProbeStatus::kIndicationRejected && o.data.ok &&
      o.data.error_status == kQmiProtocolErrorNoEntry && o.data.error_code == 0)
    return ConfigPresence::kAbsent;
  return ConfigPresence::kUnknown;
}
bool CarrierInfoVerified(CarrierTarget t,const GetConfigInfoOutcome& o) {
 const auto& d=o.data;
 return CarrierPresence(o)==ConfigPresence::kExists && d.has_total_size&&d.has_description&&d.has_version&&
   guacamole_catalog::Metadata(static_cast<int>(t),d.total_size,d.version,d.description);
}
bool VerifyCarrierBytes(CarrierTarget t, const std::vector<uint8_t>& bytes) {
  const auto* p = CarrierFileProfile(t);
  if (!p || bytes.size() != p->expected_size || bytes.empty()) return false;
  const auto sha1 = Sha1(bytes.data(), bytes.size());
  return std::vector<uint8_t>(sha1.begin(), sha1.end()) == CarrierId(t) &&
      Sha256(bytes.data(), bytes.size()) == p->expected_sha256;
}
bool CarrierChunkAccepted(const LoadConfigChunkOutcome& c, size_t length, size_t remaining) {
  return c.status == ProbeStatus::kOk && c.data.ok && c.data.error_status == 0 && c.data.error_code == 0 &&
      c.data.has_remaining_size && c.data.remaining_size == remaining &&
      (!c.data.has_received || c.data.received == length) &&
      (!c.data.has_frame_reset || !c.data.frame_reset);
}
bool H41LoadCandidate(CarrierTarget t, const CarrierReport& r, const CarrierFile& file) {
  if (t != CarrierTarget::kKt && t != CarrierTarget::kLgu) return false;
  auto is41 = [](const GetConfigInfoOutcome& o) {
    return o.status == ProbeStatus::kIndicationRejected && o.data.ok &&
        o.data.error_status == 41 && o.data.error_code == 0;
  };
  if (!r.pre_observed || !CommonIdle(r.pre) || !r.list_observed ||
      r.list.status != ProbeStatus::kOk || !r.list.data.ok || r.list.data.truncated ||
      r.list.data.error_status || r.list.data.error_code || !r.synthetic_observed ||
      !is41(r.synthetic) || !r.queried[Index(t)] || !is41(r.info[Index(t)]) ||
      !r.queried[0] || !r.queried[1] || !CarrierInfoVerified(CarrierTarget::kCommon,r.info[0]) ||
      !CarrierInfoVerified(CarrierTarget::kSkt,r.info[1])) return false;
  for (const auto& entry : r.list.data.configs)
    if (entry.type == ConfigType::kSoftware &&
        (entry.id == CarrierId(t) || entry.id == PresenceDiagnosticId())) return false;
  const auto* p = CarrierFileProfile(t);
  return file.verified && p && file.bytes.size() == p->expected_size;
}
CarrierReport RunCarrierExperiment(ICarrierClient& client, const CarrierRequest& req,
    const CarrierFile& file, const std::function<void(const CarrierReport&)>& before_mutation) {
  CarrierReport r;
  if (!ValidCarrierRequest(req)) { r.reason = "invalid command/confirmation"; return r; }
  auto info = [&](CarrierTarget t) {
    r.info[Index(t)] = client.Info(t); r.queried[Index(t)] = true;
  };
  if (req.operation == CarrierOperation::kInspect || req.operation == CarrierOperation::kInspectAll) {
    for (auto t : kTargets) if (req.operation == CarrierOperation::kInspectAll || t == req.target) info(t);
    r.outcome = "READ_ONLY_OBSERVED"; return r;
  }
  r.pre = client.Selected(); r.pre_observed = true;
  if (req.operation == CarrierOperation::kSelected) { r.outcome = "READ_ONLY_OBSERVED"; return r; }
  if (req.operation == CarrierOperation::kH41Load) {
    r.list = client.List(); r.list_observed = true;
  }
  info(CarrierTarget::kCommon); info(CarrierTarget::kSkt);
  if (req.target != CarrierTarget::kCommon) info(req.target);
  const auto& target = r.info[Index(req.target)];
  if (req.operation == CarrierOperation::kLoad || req.operation == CarrierOperation::kH41Load) {
    if (req.operation == CarrierOperation::kH41Load) {
      r.synthetic = client.SyntheticInfo(); r.synthetic_observed = true;
    }
    if (CarrierPresence(target) == ConfigPresence::kExists) {
      r.outcome = CarrierInfoVerified(req.target, target) ? "ALREADY_PRESENT_NOOP" : "LOAD_BLOCKED_METADATA";
      return r;
    }
    if (CarrierPresence(target) == ConfigPresence::kAbsent) {
      r.load_gate = "ABSENT_PROTOCOL_CONFIRMED";
    } else if (req.operation == CarrierOperation::kH41Load && H41LoadCandidate(req.target,r,file)) {
      r.load_gate = "UNREGISTERED_CANDIDATE_H41";
    } else {
      r.load_gate = "UNKNOWN";
      r.outcome = "LOAD_BLOCKED_UNKNOWN_PRESTATE"; return r;
    }
    if (!CommonIdle(r.pre) || !CarrierInfoVerified(CarrierTarget::kCommon, r.info[0]) ||
        !CarrierInfoVerified(CarrierTarget::kSkt, r.info[1])) {
      r.reason = "requires verified Common idle and SKT metadata"; return r;
    }
    const auto* p = CarrierFileProfile(req.target);
    if (!file.verified || !p || file.bytes.size() != p->expected_size) {
      r.outcome = "LOAD_BLOCKED_FILE"; r.reason = file.error; return r;
    }
    if (before_mutation) before_mutation(r);
    for (size_t offset = 0; offset < file.bytes.size();) {
      const size_t length = std::min(kLoadChunkSize, file.bytes.size() - offset);
      r.mutation_attempted = true; ++r.chunks_attempted;
      r.last_chunk = client.Chunk(req.target, offset); // one send per offset, never retry
      if (!CarrierChunkAccepted(r.last_chunk, length, file.bytes.size() - offset - length)) break;
      offset += length;
      r.transfer_completed = offset == file.bytes.size();
    }
    r.post_info = client.Info(req.target); r.post_info_observed = true;
    r.post = client.Selected(); r.post_observed = true;
    r.load_post_verified = r.transfer_completed && CarrierInfoVerified(req.target, r.post_info) &&
        GoodSelected(r.post) && r.pre.data.active_id == r.post.data.active_id &&
        r.pre.data.pending_id == r.post.data.pending_id;
    r.outcome = "USER_REVIEW_REQUIRED";
    r.reason = "single logical Load attempted; inspect chunk status, exact post info and selected; no retry/select";
    return r;
  }
  // Runtime evidence, not compile-time membership, admits KT/LGU to recovery.
  if (req.target == CarrierTarget::kCommon) {
    for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu})
      if (r.pre.data.active_id == CarrierId(t) || r.pre.data.pending_id == CarrierId(t)) info(t);
  }
  if (!GoodSelected(r.pre) || !CarrierInfoVerified(CarrierTarget::kCommon, r.info[0]) ||
      !CarrierInfoVerified(CarrierTarget::kSkt, r.info[1]) || !CarrierInfoVerified(req.target, target) ||
      !VerifiedId(r.pre.data.active_id, r) ||
      (r.pre.data.pending_id && !VerifiedId(r.pre.data.pending_id, r))) {
    r.outcome = "SELECTION_BLOCKED_UNVERIFIED_STATE"; return r;
  }
  if (req.target == CarrierTarget::kCommon && CommonIdle(r.pre)) { r.outcome = "NOOP_OK"; return r; }
  // Fresh carrier selection starts only from Common/NONE. Recovery handles known pending states.
  if (req.target != CarrierTarget::kCommon && !CommonIdle(r.pre)) {
    r.outcome = "SELECTION_BLOCKED_NONIDLE"; return r;
  }
  if (before_mutation) before_mutation(r);
  r.mutation_attempted = true;
  r.selection = client.Select(req.target);
  r.post = client.Selected(); r.post_observed = true;
  r.outcome = "USER_REVIEW_REQUIRED";
  r.reason = "one SetSelected attempted; post-state is observation only; no auto recovery/activation/retry";
  return r;
}
}
