#include "pdc_client.h"

#include <chrono>
#include <cstdio>

namespace guacamole_pdc {

namespace {
constexpr uint32_t kInitTimeoutMs = 5000;
}  // namespace

const char* ProbeStatusToString(ProbeStatus status) {
  switch (status) {
    case ProbeStatus::kOk: return "OK";
    case ProbeStatus::kInitFailed: return "INIT_FAILED";
    case ProbeStatus::kRequestSendFailed: return "REQUEST_SEND_FAILED";
    case ProbeStatus::kRequestRejected: return "REQUEST_REJECTED";
    case ProbeStatus::kTimeout: return "TIMEOUT";
    case ProbeStatus::kMalformedResponse: return "MALFORMED_RESPONSE";
    case ProbeStatus::kIndicationRejected: return "INDICATION_REJECTED";
  }
  return "UNKNOWN";
}

PdcClient::~PdcClient() {
  if (handle_ != nullptr) {
    shim_.ClientRelease(handle_);
    handle_ = nullptr;
  }
}

bool PdcClient::Init(const std::vector<VersionTriple>& version_candidates,
                      std::string* log) {
  std::string err;
  if (!shim_.Load(&err)) {
    *log = "shim load failed: " + err;
    return false;
  }

  for (const auto& v : version_candidates) {
    QmiServiceObject obj = shim_.PdcGetServiceObject(v.major, v.minor, v.tool);
    if (obj == nullptr) {
      *log += "version (" + std::to_string(v.major) + "," +
              std::to_string(v.minor) + "," + std::to_string(v.tool) +
              ") -> null service object\n";
      continue;
    }
    QmiClientHandle handle = nullptr;
    int rc = shim_.ClientInitInstance(obj, &PdcClient::IndicationTrampoline,
                                       this, kInitTimeoutMs, &handle);
    if (rc != kQmiNoErr || handle == nullptr) {
      *log += "version (" + std::to_string(v.major) + "," +
              std::to_string(v.minor) + "," + std::to_string(v.tool) +
              ") -> service object ok, qmi_client_init_instance failed (rc=" +
              std::to_string(rc) + ")\n";
      continue;
    }
    handle_ = handle;
    *log += "version (" + std::to_string(v.major) + "," +
            std::to_string(v.minor) + "," + std::to_string(v.tool) +
            ") -> OK\n";
    return true;
  }

  *log += "all version candidates failed";
  return false;
}

void PdcClient::IndicationTrampoline(QmiClientHandle /*handle*/,
                                      unsigned int msg_id, void* ind_buf,
                                      unsigned int ind_buf_len,
                                      void* ind_cb_data) {
  auto* self = reinterpret_cast<PdcClient*>(ind_cb_data);
  self->OnIndication(msg_id, reinterpret_cast<const uint8_t*>(ind_buf),
                      ind_buf_len);
}

void PdcClient::OnIndication(unsigned int msg_id, const uint8_t* buf,
                              size_t len) {
  // Deliberately minimal work on this callback's calling context (which
  // QCCI's own thread owns, not ours): parse only far enough to learn the
  // Token, copy the raw bytes out, wake whoever is waiting. All real
  // parsing happens back on the caller's thread in ListConfigs()/
  // GetConfigInfo(), never here.
  if (msg_id != kMsgListConfigs && msg_id != kMsgGetConfigInfo) return;
  auto token = PeekIndicationToken(buf, len);
  if (!token) return;  // can't route it; drop rather than guess.

  std::vector<uint8_t> copy(buf, buf + len);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_[*token] = std::move(copy);
  }
  cv_.notify_all();
}

bool PdcClient::WaitForIndication(uint32_t token, uint32_t timeout_ms,
                                   std::vector<uint8_t>* out_bytes) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool found = cv_.wait_until(lock, deadline, [&] {
    return pending_.find(token) != pending_.end();
  });
  if (!found) return false;
  *out_bytes = std::move(pending_[token]);
  pending_.erase(token);
  return true;
}

ListConfigsOutcome PdcClient::ListConfigs(uint32_t timeout_ms) {
  ListConfigsOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }

  const uint32_t token = next_token_++;
  auto req = BuildListConfigsRequest(token, ConfigType::kSoftware);

  uint8_t sync_ack[64];
  unsigned int recv_len = 0;
  int rc = shim_.SendRawMsgSync(handle_, kMsgListConfigs, req.data(),
                                 static_cast<unsigned int>(req.size()),
                                 sync_ack, sizeof(sync_ack), &recv_len,
                                 timeout_ms);
  if (rc != kQmiNoErr) {
    outcome.status = ProbeStatus::kRequestSendFailed;
    return outcome;
  }
  uint16_t est = 0, ecode = 0;
  if (!ParseOperationResultOnly(sync_ack, recv_len, &est, &ecode)) {
    // The sync ack itself didn't decode (missing/malformed Operation
    // Result TLV) - this is a distinct failure from "decoded, and it
    // said reject". Bail out now; do NOT fall through to waiting for an
    // indication that a malformed ack gives no real reason to expect.
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }
  if (est != 0) {
    outcome.status = ProbeStatus::kRequestRejected;
    outcome.data.error_status = est;
    outcome.data.error_code = ecode;
    return outcome;
  }

  std::vector<uint8_t> ind_bytes;
  if (!WaitForIndication(token, timeout_ms, &ind_bytes)) {
    outcome.status = ProbeStatus::kTimeout;
    return outcome;
  }

  outcome.data =
      ParseListConfigsIndication(ind_bytes.data(), ind_bytes.size(), token);
  switch (ClassifyListConfigsResult(outcome.data)) {
    case IndicationOutcome::kSuccess:
      outcome.status = ProbeStatus::kOk;
      break;
    case IndicationOutcome::kRejected:
      outcome.status = ProbeStatus::kIndicationRejected;
      break;
    case IndicationOutcome::kMalformed:
      outcome.status = ProbeStatus::kMalformedResponse;
      break;
  }
  return outcome;
}

GetConfigInfoOutcome PdcClient::GetConfigInfo(ConfigType type,
                                               const std::vector<uint8_t>& id,
                                               uint32_t timeout_ms) {
  GetConfigInfoOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }
  if (id.size() > kMaxIdLen) {
    outcome.status = ProbeStatus::kMalformedResponse;  // reject before send.
    return outcome;
  }

  const uint32_t token = next_token_++;
  auto req = BuildGetConfigInfoRequest(token, type, id);
  if (req.empty()) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }

  uint8_t sync_ack[64];
  unsigned int recv_len = 0;
  int rc = shim_.SendRawMsgSync(handle_, kMsgGetConfigInfo, req.data(),
                                 static_cast<unsigned int>(req.size()),
                                 sync_ack, sizeof(sync_ack), &recv_len,
                                 timeout_ms);
  if (rc != kQmiNoErr) {
    outcome.status = ProbeStatus::kRequestSendFailed;
    return outcome;
  }
  uint16_t est = 0, ecode = 0;
  if (!ParseOperationResultOnly(sync_ack, recv_len, &est, &ecode)) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }
  if (est != 0) {
    outcome.status = ProbeStatus::kRequestRejected;
    outcome.data.error_status = est;
    outcome.data.error_code = ecode;
    return outcome;
  }

  std::vector<uint8_t> ind_bytes;
  if (!WaitForIndication(token, timeout_ms, &ind_bytes)) {
    outcome.status = ProbeStatus::kTimeout;
    return outcome;
  }

  outcome.data = ParseGetConfigInfoIndication(ind_bytes.data(),
                                               ind_bytes.size(), token);
  switch (ClassifyGetConfigInfoResult(outcome.data)) {
    case IndicationOutcome::kSuccess:
      outcome.status = ProbeStatus::kOk;
      break;
    case IndicationOutcome::kRejected:
      outcome.status = ProbeStatus::kIndicationRejected;
      break;
    case IndicationOutcome::kMalformed:
      outcome.status = ProbeStatus::kMalformedResponse;
      break;
  }
  return outcome;
}

}  // namespace guacamole_pdc
