// guacamole_pdc_probe - PDC client: ties qcci_shim (transport) and
// pdc_wire (codec) together into two safe, read-only, blocking calls:
// ListConfigs() and GetConfigInfo(). No Load/Activate/Deactivate/Delete
// is implemented anywhere in this class - see Phase B scope.
//
// PDC's List Configs / Get Config Info are request+indication pairs, not
// simple synchronous request/response: `qmi_client_send_raw_msg_sync`
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
#include <map>
#include <mutex>
#include <string>
#include <vector>

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

class PdcClient {
 public:
  PdcClient() = default;
  ~PdcClient();

  PdcClient(const PdcClient&) = delete;
  PdcClient& operator=(const PdcClient&) = delete;

  // Tries each candidate version triple in order until one yields a
  // non-null service object AND a successful qmi_client_init_instance.
  // Logs which one worked (or that all failed) via `*log`.
  bool Init(const std::vector<VersionTriple>& version_candidates,
            std::string* log);

  ListConfigsOutcome ListConfigs(uint32_t timeout_ms);
  GetConfigInfoOutcome GetConfigInfo(ConfigType type,
                                      const std::vector<uint8_t>& id,
                                      uint32_t timeout_ms);

 private:
  static void IndicationTrampoline(QmiClientHandle handle, unsigned int msg_id,
                                    void* ind_buf, unsigned int ind_buf_len,
                                    void* ind_cb_data);
  void OnIndication(unsigned int msg_id, const uint8_t* buf, size_t len);

  // Blocks the calling thread until `token` shows up in pending_ (posted
  // by OnIndication) or `timeout_ms` elapses. On success, moves the bytes
  // out of pending_ and returns true.
  bool WaitForIndication(uint32_t token, uint32_t timeout_ms,
                          std::vector<uint8_t>* out_bytes);

  QcciShim shim_;
  QmiClientHandle handle_ = nullptr;
  uint32_t next_token_ = 1;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint32_t, std::vector<uint8_t>> pending_;
};

}  // namespace guacamole_pdc
