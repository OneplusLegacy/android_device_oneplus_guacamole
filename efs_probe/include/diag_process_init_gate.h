#pragma once
#include <atomic>

namespace guacamole_efs {
// Real H.41 libdiag may retain process-local state after DeInit. Once any Init
// is attempted, only a new exec can provide another supported lifecycle.
// Intentionally no release/reset, including after Init failure.
class DiagProcessInitGate {
 public:
  bool Consume() {
    bool expected = false;
    return consumed_.compare_exchange_strong(expected, true);
  }
 private:
  std::atomic<bool> consumed_{false};
};
}
