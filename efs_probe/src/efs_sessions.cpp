#include <utility>
#include "efs_sessions.h"
#include <atomic>
#include "efs_real_client.h"
#include "efs_real_write_client.h"

namespace guacamole_efs {
namespace {
std::atomic<bool> gOwnedSession{false};
class SessionLease {
 public:
  SessionLease() {
    bool expected = false;
    owned_ = gOwnedSession.compare_exchange_strong(expected, true);
  }
  ~SessionLease() { if (owned_) gOwnedSession.store(false); }
  bool owned() const { return owned_; }
 private:
  bool owned_;
};
class ReadSession final : public IEfsReadSession {
 public:
  ReadSession(std::unique_ptr<SessionLease> lease, std::unique_ptr<DiagTransport> transport,
      std::chrono::milliseconds timeout)
      : lease_(std::move(lease)), transport_(std::move(transport)),
        client_(transport_.get(), timeout) {}
  IEfsClient& client() override { return client_; }
 private:
  // Reverse destruction order: client is gone before transport/registered
  // callback context is destroyed; DiagTransport::~DiagTransport joins Deinit.
  std::unique_ptr<SessionLease> lease_;
  std::unique_ptr<DiagTransport> transport_;
  EfsRealClient client_;
};
class WriteSession final : public IEfsWriteSession {
 public:
  WriteSession(std::unique_ptr<SessionLease> lease, std::unique_ptr<DiagTransport> transport,
      std::chrono::milliseconds timeout)
      : lease_(std::move(lease)), transport_(std::move(transport)),
        client_(transport_.get(), timeout) {}
  IEfsWriteClient& client() override { return client_; }
 private:
  std::unique_ptr<SessionLease> lease_;
  std::unique_ptr<DiagTransport> transport_;
  EfsRealWriteClient client_;
};
std::unique_ptr<DiagTransport> OpenTransport(
    const DiagEfsSessionFactory::ShimFactory& make_shim, std::string* error) {
  auto shim = make_shim ? make_shim() : nullptr;
  if (!shim) {
    if (error) *error = "no DIAG shim available";
    return nullptr;
  }
  auto transport = std::make_unique<DiagTransport>(std::move(shim),
      DiagFrameMode::kH41Framed);
  if (!transport->Init(error)) return nullptr;
  return transport;
}
}  // namespace
DiagEfsSessionFactory::DiagEfsSessionFactory(ShimFactory make_shim,
    std::chrono::milliseconds timeout)
    : make_shim_(std::move(make_shim)), timeout_(timeout) {}
std::unique_ptr<IEfsReadSession> DiagEfsSessionFactory::OpenRead(std::string* error) {
  auto lease = std::make_unique<SessionLease>();
  if (!lease->owned()) {
    if (error) *error = "another owned DIAG session is still alive";
    return nullptr;
  }
  auto transport = OpenTransport(make_shim_, error);
  if (!transport) return nullptr;
  return std::make_unique<ReadSession>(std::move(lease), std::move(transport), timeout_);
}
std::unique_ptr<IEfsWriteSession> DiagEfsSessionFactory::OpenWrite(std::string* error) {
  auto lease = std::make_unique<SessionLease>();
  if (!lease->owned()) {
    if (error) *error = "another owned DIAG session is still alive";
    return nullptr;
  }
  auto transport = OpenTransport(make_shim_, error);
  if (!transport) return nullptr;
  return std::make_unique<WriteSession>(std::move(lease), std::move(transport), timeout_);
}
}  // namespace guacamole_efs
