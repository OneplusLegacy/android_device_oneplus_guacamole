#include "efs_real_client.h"

namespace guacamole_efs {

namespace {
// Copies the transport-level raw/framing metadata into a wire call log
// entry - shared by every method below, regardless of whether the call
// went on to succeed, fail transport-side, or fail frame integrity.
void CopyTransportMetadata(const TransportResult& result, EfsWireCallLog* log) {
  log->transport_status = result.status;
  log->raw_response = result.raw_response;
  log->had_hdlc_trailer = result.had_hdlc_trailer;
  log->crc_valid = result.crc_valid;
  log->crc_expected = result.crc_expected;
  log->crc_received = result.crc_received;
  log->escape_present = result.escape_present;
  log->escape_count = result.escape_count;
  log->unescaped_len = result.unescaped_len;
}
}  // namespace

EfsRealClient::EfsRealClient(DiagTransport* transport,
                              std::chrono::milliseconds timeout)
    : transport_(transport), timeout_(timeout) {}

EfsStatResult EfsRealClient::Stat(const std::string& path) {
  EfsWireCallLog log;
  log.label = "STAT";
  auto request = BuildStatRequest(path);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsStatResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsStatResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseStatResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsOpenResult EfsRealClient::OpenReadOnly(const std::string& path) {
  EfsWireCallLog log;
  log.label = "OPEN";
  auto request = BuildOpenReadOnlyRequest(path, kOflagReadOnly);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsOpenResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsOpenResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseOpenResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsReadResult EfsRealClient::Read(int32_t fd, uint32_t nbyte,
                                   uint32_t offset) {
  EfsWireCallLog log;
  log.label = "READ";
  auto request = BuildReadRequest(fd, nbyte, offset);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsReadResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsReadResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseReadResponse(result.response.data(), result.response.size(),
                                   fd, offset, nbyte);
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsCloseResult EfsRealClient::Close(int32_t fd) {
  EfsWireCallLog log;
  log.label = "CLOSE";
  auto request = BuildCloseRequest(fd);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsCloseResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsCloseResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseCloseResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.errno_value;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsOpenDirResult EfsRealClient::OpenDir(const std::string& path) {
  EfsWireCallLog log;
  log.label = "OPENDIR";
  auto request = BuildOpenDirRequest(path);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsOpenDirResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsOpenDirResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseOpenDirResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsReadDirResult EfsRealClient::ReadDir(uint32_t dirp, int32_t seqno) {
  EfsWireCallLog log;
  log.label = "READDIR";
  auto request = BuildReadDirRequest(dirp, seqno);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsReadDirResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsReadDirResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseReadDirResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return parsed;
}

EfsCloseDirResult EfsRealClient::CloseDir(uint32_t dirp) {
  EfsWireCallLog log;
  log.label = "CLOSEDIR";
  auto request = BuildCloseDirRequest(dirp);
  log.request = request;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return EfsCloseDirResult{};
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return EfsCloseDirResult{};
  }
  log.logical_response = result.response;
  auto parsed = ParseCloseDirResponse(result.response.data(), result.response.size());
  log.parsed_ok = parsed.ok;
  log.diag_errno = parsed.errno_value;
  call_log_.push_back(std::move(log));
  return parsed;
}

}  // namespace guacamole_efs
