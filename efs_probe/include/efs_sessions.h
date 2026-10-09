// Owned, single-purpose DIAG sessions. Session destruction quiesces callbacks
// and deinitializes the transport. This does NOT reset vendor process state.
// Real H.41 Init is process-once; fresh device phases require separate execs.
// The object factory alone is not production transaction isolation.
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "efs_client.h"
#include "efs_write_client.h"

namespace guacamole_efs {
class IEfsReadSession {
 public:
  virtual ~IEfsReadSession() = default;
  virtual IEfsClient& client() = 0;
};
class IEfsWriteSession {
 public:
  virtual ~IEfsWriteSession() = default;
  virtual IEfsWriteClient& client() = 0;
};
class IEfsSessionFactory {
 public:
  virtual ~IEfsSessionFactory() = default;
  virtual std::unique_ptr<IEfsReadSession> OpenRead(std::string* error) = 0;
  virtual std::unique_ptr<IEfsWriteSession> OpenWrite(std::string* error) = 0;
};

// Real transport/client implementation; injected shim creator permits host
// tests without libdiag. The read-only worker supplies DiagCallbackShim.
// Production Binder still calls none of this. Always H41_FRAMED, never raw.
class DiagEfsSessionFactory final : public IEfsSessionFactory {
 public:
  using ShimFactory = std::function<std::unique_ptr<IDiagCallbackShim>()>;
  explicit DiagEfsSessionFactory(ShimFactory make_shim,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
  std::unique_ptr<IEfsReadSession> OpenRead(std::string* error) override;
  std::unique_ptr<IEfsWriteSession> OpenWrite(std::string* error) override;
 private:
  ShimFactory make_shim_;
  std::chrono::milliseconds timeout_;
};
}  // namespace guacamole_efs
