#include "diag_callback_shim.h"
#include "diag_process_init_gate.h"

// Not dlopen: same rationale as qcci_shim.cpp/diag_transport.cpp -
// libdiag.so already has a real cc_prebuilt_library_shared module in
// this tree, so this probe links against it directly. See
// diag_callback_shim.h for the full provenance of every signature here.
extern "C" {
int Diag_LSM_Init(uint8_t* param);
void Diag_LSM_DeInit(void);
void diag_register_callback(guacamole_efs::DiagDataCallback cb,
                             void* context);
void diag_switch_logging(int mode, const char* mask);
int diag_callback_send_data(int proc, void* buf, int len);
}

namespace guacamole_efs {
namespace { DiagProcessInitGate gLibdiagInit; }

DiagCallbackShim::~DiagCallbackShim() { Deinit(); }

bool DiagCallbackShim::Init(std::string* error) {
  if (!gLibdiagInit.Consume()) {
    if (error) *error = "libdiag Init already attempted; fresh OS process required";
    return false;
  }
  int rc = Diag_LSM_Init(nullptr);
  if (rc == 0) {
    if (error) *error = "Diag_LSM_Init returned 0 (failure)";
    return false;
  }
  owns_session_ = true;
  return true;
}

void DiagCallbackShim::Deinit() {
  if (!owns_session_) return;
  Diag_LSM_DeInit();
  owns_session_ = false;
}

bool DiagCallbackShim::RegisterCallback(DiagDataCallback cb, void* context,
                                         std::string* error) {
  if (!owns_session_) {
    if (error) *error = "no initialized libdiag session";
    return false;
  }
  if (cb == nullptr) {
    if (error) *error = "null callback";
    return false;
  }
  // `context` is stored by value inside libdiag and echoed back later -
  // see diag_callback_shim.h's header comment for the disassembly
  // evidence. The caller (DiagTransport) is responsible for passing
  // something long-lived (its own `this`), never a local variable's
  // address.
  diag_register_callback(cb, context);
  // No confirmed failure signal from this call - see diag_callback_shim.h.
  // If it didn't crash, treat it as succeeded, matching the reference
  // sample's own behavior of never checking it.
  return true;
}

bool DiagCallbackShim::SwitchLogging(int mode, std::string* error) {
  if (!owns_session_) {
    if (error) *error = "no initialized libdiag session";
    return false;
  }
  diag_switch_logging(mode, nullptr);
  // Void-returning (PUBLIC_REFERENCE_CONFIRMED) - no failure signal
  // available here either.
  return true;
}

bool DiagCallbackShim::SendData(int proc, const void* buf, int len,
                                 std::string* error) {
  if (!owns_session_) {
    if (error) *error = "no initialized libdiag session";
    return false;
  }
  int rc = diag_callback_send_data(proc, const_cast<void*>(buf), len);
  if (rc != 0) {
    if (error) *error = "diag_callback_send_data returned " +
                         std::to_string(rc);
    return false;
  }
  return true;
}

}  // namespace guacamole_efs
