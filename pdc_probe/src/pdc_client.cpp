#include "pdc_client.h"

#include <chrono>
#include <cstdio>
#include <limits>

namespace guacamole_pdc {

namespace {
constexpr uint32_t kInitTimeoutMs = 5000;
}  // namespace

// Vendor symbols live only in qcci_shim.cpp; this client is host-testable.

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
  if (msg_id != kMsgListConfigs && msg_id != kMsgGetConfigInfo &&
      msg_id != kMsgGetSelectedConfig && msg_id != kMsgLoadConfig &&
      msg_id != kMsgSetSelectedConfig)
    return;
  auto token = PeekIndicationToken(buf, len);
  if (!token) return;  // can't route it; drop rather than guess.

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!expected_ || expected_->first != msg_id ||
        expected_->second != *token || pending_) return;
    pending_ = std::vector<uint8_t>(buf, buf + len);
  }
  cv_.notify_all();
}

PdcClient::RequestScope::RequestScope(PdcClient& client, unsigned int msg_id,
                                      uint32_t token) : client_(client) {
  std::lock_guard<std::mutex> lock(client_.mutex_);
  client_.pending_.reset();
  client_.expected_ = std::make_pair(msg_id, token);
}

PdcClient::RequestScope::~RequestScope() {
  std::lock_guard<std::mutex> lock(client_.mutex_);
  client_.expected_.reset();
  client_.pending_.reset();  // also on sync rejection / malformed ACK / timeout
}

bool PdcClient::WaitForIndication(uint32_t timeout_ms,
                                  std::vector<uint8_t>* out_bytes) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  if (!cv_.wait_until(lock, deadline, [&] { return pending_.has_value(); }))
    return false;
  *out_bytes = std::move(*pending_);
  // Retain the occupied slot until RequestScope ends, rejecting duplicates.
  return true;
}

ListConfigsOutcome PdcClient::ListConfigs(uint32_t timeout_ms) {
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  ListConfigsOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }

  if (next_token_ > std::numeric_limits<uint32_t>::max()) {
    outcome.status = ProbeStatus::kRequestSendFailed;  // no token reuse/send
    return outcome;
  }
  const uint32_t token = static_cast<uint32_t>(next_token_++);
  auto req = BuildListConfigsRequest(token, ConfigType::kSoftware);

  RequestScope request(*this, kMsgListConfigs, token);
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
  if (!WaitForIndication(timeout_ms, &ind_bytes)) {
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
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  GetConfigInfoOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }
  if (id.size() > kMaxIdLen) {
    outcome.status = ProbeStatus::kMalformedResponse;  // reject before send.
    return outcome;
  }

  if (next_token_ > std::numeric_limits<uint32_t>::max()) {
    outcome.status = ProbeStatus::kRequestSendFailed;  // no token reuse/send
    return outcome;
  }
  const uint32_t token = static_cast<uint32_t>(next_token_++);
  auto req = BuildGetConfigInfoRequest(token, type, id);
  if (req.empty()) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }

  RequestScope request(*this, kMsgGetConfigInfo, token);
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
  if (!WaitForIndication(timeout_ms, &ind_bytes)) {
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

GetSelectedConfigOutcome PdcClient::GetSelectedConfig(ConfigType type,
                                                        uint32_t timeout_ms) {
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  GetSelectedConfigOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }

  if (next_token_ > std::numeric_limits<uint32_t>::max()) {
    outcome.status = ProbeStatus::kRequestSendFailed;  // no token reuse/send
    return outcome;
  }
  const uint32_t token = static_cast<uint32_t>(next_token_++);
  auto req = BuildGetSelectedConfigRequest(token, type);

  RequestScope request(*this, kMsgGetSelectedConfig, token);
  uint8_t sync_ack[64];
  unsigned int recv_len = 0;
  int rc = shim_.SendRawMsgSync(handle_, kMsgGetSelectedConfig, req.data(),
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
  if (!WaitForIndication(timeout_ms, &ind_bytes)) {
    outcome.status = ProbeStatus::kTimeout;
    return outcome;
  }

  outcome.data = ParseGetSelectedConfigIndication(ind_bytes.data(),
                                                   ind_bytes.size(), token);
  switch (ClassifyGetSelectedConfigResult(outcome.data)) {
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

LoadConfigChunkOutcome PdcClient::LoadConfigChunk(
    ConfigType type, const std::vector<uint8_t>& id, uint32_t total_size,
    const uint8_t* chunk_bytes, size_t chunk_len, uint32_t timeout_ms) {
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  LoadConfigChunkOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }

  if (next_token_ > std::numeric_limits<uint32_t>::max()) {
    outcome.status = ProbeStatus::kRequestSendFailed;  // no token reuse/send
    return outcome;
  }
  const uint32_t token = static_cast<uint32_t>(next_token_++);
  auto req = BuildLoadConfigChunkRequest(token, type, id, total_size,
                                          chunk_bytes, chunk_len);
  if (req.empty()) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }

  RequestScope request(*this, kMsgLoadConfig, token);
  uint8_t sync_ack[64];
  unsigned int recv_len = 0;
  int rc = shim_.SendRawMsgSync(handle_, kMsgLoadConfig, req.data(),
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
  if (!WaitForIndication(timeout_ms, &ind_bytes)) {
    // Per Phase C0 policy: a timeout here does NOT mean "resend this
    // chunk" - the caller (skt_load.cpp's state machine) is responsible
    // for treating this as UNKNOWN_RESULT and re-querying List Configs
    // to determine the real outcome, not retrying blindly.
    outcome.status = ProbeStatus::kTimeout;
    return outcome;
  }

  outcome.data =
      ParseLoadConfigIndication(ind_bytes.data(), ind_bytes.size(), token);
  switch (ClassifyLoadConfigChunkResult(outcome.data)) {
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

// Controlled debug experiment only - see IPdcClient::SetSelectedConfig()
// (pdc_client.h) and pdc_select_config_wire.h's header comment. Mirrors
// GetConfigInfo()'s exact structure above - request+sync-ack+indication,
// same as every other PDC call in this class.
SetSelectedConfigOutcome PdcClient::SetSelectedConfig(
    ConfigType type, const std::vector<uint8_t>& id, uint32_t timeout_ms) {
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  SetSelectedConfigOutcome outcome;
  if (handle_ == nullptr) {
    outcome.status = ProbeStatus::kInitFailed;
    return outcome;
  }
  if (id.empty() || id.size() > kMaxIdLen ||
      (type != ConfigType::kPlatform && type != ConfigType::kSoftware)) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }

  if (next_token_ > std::numeric_limits<uint32_t>::max()) {
    outcome.status = ProbeStatus::kRequestSendFailed;  // no token reuse/send
    return outcome;
  }
  const uint32_t token = static_cast<uint32_t>(next_token_++);
  auto req = BuildSetSelectedConfigRequest(token, type, id);
  if (req.empty()) {
    outcome.status = ProbeStatus::kMalformedResponse;
    return outcome;
  }

  RequestScope request(*this, kMsgSetSelectedConfig, token);
  uint8_t sync_ack[64];
  unsigned int recv_len = 0;
  int rc = shim_.SendRawMsgSync(handle_, kMsgSetSelectedConfig, req.data(),
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
  if (!WaitForIndication(timeout_ms, &ind_bytes)) {
    outcome.status = ProbeStatus::kTimeout;
    return outcome;
  }

  outcome.data = ParseSetSelectedConfigIndication(ind_bytes.data(),
                                                    ind_bytes.size(), token);
  switch (ClassifySetSelectedConfigResult(outcome.data)) {
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
