// guacamole_pdc_probe - PDC client: ties qcci_shim (transport) and
// pdc_wire (codec) together into blocking operations. Read-only CLI calls
// use List/GetInfo/GetSelected. Load is the existing gated probe operation;
// SetSelected is exposed only by the separate controlled debug experiment.
//
// PDC's List Configs / Get Config Info / Get Selected Config are
// request+indication pairs, not simple synchronous request/response:
// `qmi_client_send_raw_msg_sync`
// only gets you the immediate "request accepted" ack (an Operation Result
// TLV); the actual answer arrives later via the indication callback
// registered at qmi_client_init_instance() time, correlated by the Token TLV we
// put in our own request. This class owns that correlation, with a
// bounded wait and no work done inside the indication callback itself
// beyond copying bytes and waking the waiter - this mirrors the DCI
// buffer-lifetime/threading concerns Codex's review raised for the
// diag DCI path, which apply identically here.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

#include "pdc_load_wire.h"
#include "pdc_select_config_wire.h"
#include "pdc_wire.h"
#include "qcci_shim.h"

namespace guacamole_pdc {

enum class ProbeStatus {
  kOk,
  kInitFailed,          // dlopen/dlsym/service-object/client-init failure
  kRequestSendFailed,   // qmi_client_send_raw_msg_sync itself errored
  kRequestRejected,     // sync ack's Operation Result was non-zero
  kTimeout,             // no matching indication within the deadline
  kMalformedResponse,   // sync ack OR indication failed to decode safely
  kIndicationRejected,  // indication decoded fine, but the modem's own
                        // Indication Result TLV was non-zero - distinct
                        // from kMalformedResponse (untrusted bytes) and
                        // from kRequestRejected (rejected at the earlier
                        // synchronous-ack stage, before any indication
                        // ever arrived).
};

const char* ProbeStatusToString(ProbeStatus status);

struct ListConfigsOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  ListConfigsResult data;
};

struct GetConfigInfoOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  GetConfigInfoResult data;
};

// Whether a specific config ID actually exists in the modem's PDC
// database - determined EXCLUSIVELY from Get Config Info against that
// exact ID, never from List Configs. This distinction is load-bearing:
// a real-device Phase C0 test found that List Configs can silently omit
// an entry that Get Config Info still successfully resolves (a
// pre-existing config vanished from one List Configs call and
// reappeared in the next, while Get Config Info on its exact ID
// succeeded throughout - see PHASE_C0_FINDINGS.md's "real-device test
// #2" section). List Configs is diagnostic-only from here on; it must
// never be the basis for a presence/absence decision.
enum class ConfigPresence {
  kExists,   // Get Config Info succeeded for this exact ID.
  kAbsent,   // Get Config Info failed with the specific, well-evidenced
             // QMI_PROTOCOL_ERROR_NO_ENTRY (see pdc_wire.h's
             // kQmiProtocolErrorNoEntry) - a real "no such id" answer.
  kUnknown,  // Anything else: timeout, malformed response, a request-
             // stage rejection, or an indication-stage rejection with
             // any OTHER error code. Deliberately fail-closed - callers
             // must treat kUnknown the same as "cannot proceed safely",
             // never the same as kAbsent.
};

const char* ConfigPresenceToString(ConfigPresence presence);

// The sole authoritative presence classifier - see ConfigPresence's own
// comment for why. Implemented in pdc_wire.cpp (not pdc_client.cpp) so
// it links into the vendor-independent host test target, matching
// ProbeStatusToString()'s placement for the same reason.
ConfigPresence ClassifyConfigPresence(const GetConfigInfoOutcome& outcome);

struct GetSelectedConfigOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  GetSelectedConfigResult data;
};

struct LoadConfigChunkOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  LoadConfigChunkResult data;
};

struct SetSelectedConfigOutcome {
  ProbeStatus status = ProbeStatus::kInitFailed;
  SetSelectedConfigResult data;
};

// Abstract transport-level interface, split out purely so
// skt_load.cpp's state machine can be host-unit-tested against a fake
// implementation (tests/skt_load_test.cpp) instead of the real
// dlopen-free-but-still-real-vendor-library PdcClient, which needs an
// actual device to do anything. Every method here has the exact same
// contract as the corresponding PdcClient method below - this is a pure
// extraction, not a redesign.
class IPdcClient {
 public:
  virtual ~IPdcClient() = default;
  virtual ListConfigsOutcome ListConfigs(uint32_t timeout_ms) = 0;
  virtual GetConfigInfoOutcome GetConfigInfo(ConfigType type,
                                              const std::vector<uint8_t>& id,
                                              uint32_t timeout_ms) = 0;
  virtual GetSelectedConfigOutcome GetSelectedConfig(ConfigType type,
                                                      uint32_t timeout_ms) = 0;
  virtual LoadConfigChunkOutcome LoadConfigChunk(
      ConfigType type, const std::vector<uint8_t>& id, uint32_t total_size,
      const uint8_t* chunk_bytes, size_t chunk_len, uint32_t timeout_ms) = 0;

  // Round 3: only the separate debug-only mapping harness calls this on
  // device. Original pdc CLI and production daemon do not expose selection.
  // A real experiment requires exact confirmation and explicit user review.
  virtual SetSelectedConfigOutcome SetSelectedConfig(
      ConfigType type, const std::vector<uint8_t>& id, uint32_t timeout_ms) = 0;
};

class PdcClient : public IPdcClient {
 public:
  PdcClient();  // production constructor defined with the vendor shim
  // The injected shim must outlive this client, including ClientRelease().
  explicit PdcClient(IQcciShim& shim) : shim_(shim) {}
  ~PdcClient() override;

  PdcClient(const PdcClient&) = delete;
  PdcClient& operator=(const PdcClient&) = delete;

  // Tries each candidate version triple in order until one yields a
  // non-null service object AND a successful qmi_client_init_instance.
  // Logs which one worked (or that all failed) via `*log`.
  bool Init(const std::vector<VersionTriple>& version_candidates,
            std::string* log);

  ListConfigsOutcome ListConfigs(uint32_t timeout_ms) override;
  GetConfigInfoOutcome GetConfigInfo(ConfigType type,
                                      const std::vector<uint8_t>& id,
                                      uint32_t timeout_ms) override;
  GetSelectedConfigOutcome GetSelectedConfig(ConfigType type,
                                              uint32_t timeout_ms) override;

  // THE WRITE PATH (Phase C0). Sends exactly ONE Load Config chunk and
  // waits for its indication - this is a low-level, single-round-trip
  // primitive, not the full load-a-file orchestration (see skt_load.h
  // for that: chunking, safety checks, before/after selected
  // verification, and the state machine all live there, on top of this
  // primitive - deliberately not folded into PdcClient itself, to keep
  // this class's contract identical in shape for every method: one
  // request, one matching indication, one classified outcome).
  LoadConfigChunkOutcome LoadConfigChunk(ConfigType type,
                                          const std::vector<uint8_t>& id,
                                          uint32_t total_size,
                                          const uint8_t* chunk_bytes,
                                          size_t chunk_len,
                                          uint32_t timeout_ms) override;

  // Controlled debug experiment only; see IPdcClient::SetSelectedConfig().
  SetSelectedConfigOutcome SetSelectedConfig(ConfigType type,
                                              const std::vector<uint8_t>& id,
                                              uint32_t timeout_ms) override;

 private:
  static void IndicationTrampoline(QmiClientHandle handle, unsigned int msg_id,
                                    void* ind_buf, unsigned int ind_buf_len,
                                    void* ind_cb_data);
  void OnIndication(unsigned int msg_id, const uint8_t* buf, size_t len);

  // One synchronous operation per client. Arm before sending: QCCI may
  // deliver the indication before SendRawMsgSync returns its sync ACK.
  class RequestScope {
   public:
    RequestScope(PdcClient& client, unsigned int msg_id, uint32_t token);
    ~RequestScope();
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;
   private:
    PdcClient& client_;
  };
  bool WaitForIndication(uint32_t timeout_ms, std::vector<uint8_t>* out_bytes);

  std::unique_ptr<IQcciShim> owned_shim_;
  IQcciShim& shim_;
  QmiClientHandle handle_ = nullptr;
  // Never wrap/reuse a token within a client lifetime.
  uint64_t next_token_ = 1;
  std::mutex request_mutex_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<std::pair<unsigned int, uint32_t>> expected_;
  std::optional<std::vector<uint8_t>> pending_;

};

}  // namespace guacamole_pdc
