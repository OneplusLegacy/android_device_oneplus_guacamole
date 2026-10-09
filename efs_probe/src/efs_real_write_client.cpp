#include "efs_real_write_client.h"

namespace guacamole_efs {

namespace {
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

EfsRealWriteClient::EfsRealWriteClient(DiagTransport* transport,
                                        std::chrono::milliseconds timeout)
    : transport_(transport), timeout_(timeout) {}

MutationOpenResult EfsRealWriteClient::OpenWrite(EfsWriteTarget target) {
  // The ONLY place in this class (and, transitively, the only place in
  // the real write path at all) a path string is ever formed - see
  // efs_write_client.h's header comment.
  const std::string& path = TargetPath(target);

  EfsWireCallLog log;
  log.label = "OPEN(write)";
  auto request = BuildOpenWriteRequest(path);
  log.request = request;
  MutationOpenResult out;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return out;  // transport_status stays kInitFailed - refuse to send.
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  out.transport_status = result.status;
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return out;
  }
  log.logical_response = result.response;
  // Shares the read path's own ParseOpenResponse() - the OPEN response
  // shape (fd, diag_errno) is identical regardless of which oflag was
  // sent; see efs_write_wire.h's header comment for why this file
  // deliberately does not redeclare a parser for it.
  out.parsed = ParseOpenResponse(result.response.data(), result.response.size());
  log.parsed_ok = out.parsed.ok;
  log.diag_errno = out.parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return out;
}

MutationWriteResult EfsRealWriteClient::Write(int32_t fd, uint32_t offset,
                                               const std::vector<uint8_t>& data) {
  EfsWireCallLog log;
  log.label = "WRITE";
  auto request = BuildWriteRequest(fd, offset, data);
  log.request = request;
  MutationWriteResult out;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return out;
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  out.transport_status = result.status;
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return out;
  }
  log.logical_response = result.response;
  out.parsed = ParseWriteResponse(result.response.data(), result.response.size(),
                                   fd, offset,
                                   static_cast<uint32_t>(data.size()));
  log.parsed_ok = out.parsed.ok;
  log.diag_errno = out.parsed.diag_errno;
  call_log_.push_back(std::move(log));
  return out;
}

MutationCloseResult EfsRealWriteClient::Close(int32_t fd) {
  EfsWireCallLog log;
  log.label = "CLOSE";
  auto request = BuildCloseRequest(fd);  // shared with the read path.
  log.request = request;
  MutationCloseResult out;
  if (request.empty()) {
    call_log_.push_back(std::move(log));
    return out;
  }
  auto result = transport_->SendSubsysCommand(0, request, timeout_);
  CopyTransportMetadata(result, &log);
  out.transport_status = result.status;
  if (result.status != TransportStatus::kOk) {
    call_log_.push_back(std::move(log));
    return out;
  }
  log.logical_response = result.response;
  out.parsed = ParseCloseResponse(result.response.data(), result.response.size());
  log.parsed_ok = out.parsed.ok;
  log.diag_errno = out.parsed.errno_value;
  call_log_.push_back(std::move(log));
  return out;
}

}  // namespace guacamole_efs
