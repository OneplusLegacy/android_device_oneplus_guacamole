// guacamole_efs_probe - thin wrapper around the 5 libdiag.so calls the
// callback-mode client transport needs. Exists purely for
// host-testability (mirrors guacamole_pdc::QcciShim, one layer lower:
// Phase D0.5 explicitly requires host-testing the request/response
// correlation state machine itself, not just orchestration built on
// top of it - see diag_transport.h).
//
// CORRECTED 2026-09-23 (critical): `diag_register_callback`'s second
// argument was previously declared/used as an OUTPUT slot
// (`diag_register_callback(cb, &local_stack_var)`, discarding the
// value). A code review challenged this, citing the public convention
// for this family of APIs (`diag_register_callback(cb, context)` /
// `diag_register_remote_callback(cb, proc, context)`, with `context`
// echoed back as the callback's third argument). Direct disassembly of
// `diag_register_callback`'s ACTUAL BODY inside `libdiag.so` (not just
// the sample's call site, which only shows the caller's side) settles
// this definitively: at `0x188ec`, `stp x0, x1, [x9, 8]` copies BOTH
// the callback function pointer (x0) AND the second argument (x1) BY
// VALUE into a persistent global registration table - there is no
// store THROUGH x1 anywhere in this function (i.e., libdiag never
// writes to `*x1`; it only remembers the pointer VALUE itself, to hand
// back later). This is `H41_STATICALLY_CONFIRMED`: `x1` is a
// caller-owned CONTEXT pointer, not an output parameter.
//
// Consequence: the previous implementation
// (`diag_register_callback(cb, &out_handle)` with `out_handle` a local
// inside `RegisterCallback()`) registered the ADDRESS OF A STACK
// VARIABLE THAT NO LONGER EXISTS by the time any callback could fire -
// a dangling-pointer bug that would have caused undefined behavior on
// first real-device use (the reference sample avoids this only because
// its own equivalent slot lives in `main`'s frame, which never returns
// for the life of the process - not because the API is safe to use this
// way in general). Fixed by passing a genuinely long-lived pointer
// (the `DiagTransport` instance itself, `this`) as the context, and
// using it to route the callback directly - see diag_transport.h,
// which no longer needs a process-global `active_instance_` as a
// result.
//
// This also corrects the earlier reading of the callback's third
// parameter as `int *proc` (implying libdiag itself reports a "source
// processor"). It does not: the SAMPLE happens to register a zeroed
// int as ITS OWN context and interpret whatever comes back as a
// processor id BY ITS OWN CONVENTION - libdiag treats this value as
// fully opaque (confirmed by the same disassembly: it is stored and
// returned unexamined). The callback's third parameter is declared
// `void*` here accordingly. `diag_callback_send_data`'s own `proc`
// parameter (the DESTINATION for a send) is unaffected by this
// correction - it remains a real, separate, meaningful argument,
// confirmed via its own 6 checked call sites.
//
// SIGNATURE PROVENANCE (read this before changing anything - this
// project's standing rule against guessed prototypes):
//
//   - `Diag_LSM_Init(uint8_t*)` / `Diag_LSM_DeInit(void)`: unchanged
//     from the already real-device-validated `transport-info` command
//     (H41_STATICALLY_CONFIRMED symbol presence + call-site register
//     usage; PUBLIC_REFERENCE_CONFIRMED signature via
//     bcyj/android_tools_leeco_msm8996's diag_mdlog.c - "boolean
//     bInit_Success = Diag_LSM_Init(NULL);").
//
//   - `diag_register_callback(cb, context)`: `H41_STATICALLY_CONFIRMED`
//     end to end - both the caller-side shape (`diag_callback_sample`
//     0x2360: x0 = callback fn, x1 = context pointer) AND the callee's
//     own body (`libdiag.so` 0x188ac-0x189b0: copies `(x0,x1)` into a
//     global table entry via `stp x0, x1, [x9, 8]`, never writes
//     through x1). Return value: still never checked at the sample's
//     call site (w0 is overwritten by the next instruction before ever
//     being read) - `DiagCallbackShim::RegisterCallback` can only ever
//     report success, same as before this correction.
//
//   - `diag_register_remote_callback(cb, proc, context)`:
//     `H41_STATICALLY_CONFIRMED` 3-argument shape, disassembled
//     directly at `libdiag.so` 0x189b8 (`mov w19, w1` capturing the
//     PROC argument separately from context) - corroborates that
//     `proc` and `context` are genuinely distinct concepts in this API
//     family, not two names for the same thing. Not used by this probe
//     (local-only; no remote/MDM support this phase).
//
//   - `diag_switch_logging(mode, mask)`: PUBLIC_REFERENCE_CONFIRMED,
//     directly quoted from a real, independent, working public client
//     (mobile-insight/mobileinsight-mobile's `diag_revealer.c`, fetched
//     2026-09-23): `void (*diag_switch_logging)(int, const char *);`.
//     `kDiagCallbackMode = 6` (CALLBACK_MODE) is H41_STATICALLY_CONFIRMED
//     via the exact call-site literal (`diag_switch_logging(6, NULL)`).
//
//   - `diag_callback_send_data(proc, buf, len)`: H41_STATICALLY_CONFIRMED
//     signature AND 0=success convention - all 6 call sites in
//     `diag_callback_sample` check the return via `cbz w0, <success>`.
//
//   - The registered callback's own signature is
//     `void(void *buf, int len, void *context)` -
//     `H41_STATICALLY_CONFIRMED` for the first two parameters (full
//     prologue disassembly); the third parameter's TYPE is corrected to
//     `void*` per the above (was `int*`) - its underlying representation
//     (a raw pointer value, opaque to libdiag) is `H41_STATICALLY_CONFIRMED`
//     via `diag_register_callback`'s own body; that THIS project chooses
//     to put a `DiagTransport*` there is this project's own design
//     choice, not something the firmware mandates.

#pragma once

#include <cstdint>
#include <string>

namespace guacamole_efs {

// Exact match to the H41_STATICALLY_CONFIRMED registered-callback
// signature - see this file's header comment. `context` is caller-owned
// and opaque to libdiag; this project passes a `DiagTransport*`.
using DiagDataCallback = void (*)(void* buf, int len, void* context);

// CALLBACK_MODE, H41_STATICALLY_CONFIRMED via diag_callback_sample's
// own `diag_switch_logging(6, NULL)` call site.
constexpr int kDiagCallbackMode = 6;

class IDiagCallbackShim {
 public:
  virtual ~IDiagCallbackShim() = default;

  virtual bool Init(std::string* error) = 0;
  virtual void Deinit() = 0;

  // `context` is stored/echoed back verbatim by the real implementation
  // (see this file's header comment) - callers MUST pass something with
  // a lifetime that outlives every possible callback invocation, never
  // the address of a local variable inside a short-lived function.
  //
  // See this file's header comment on why this can only ever report
  // success against the real implementation (the underlying libdiag
  // call provides no confirmed failure signal) - the bool return exists
  // for fake-shim testability, not because the real wrapper can
  // meaningfully fail here today.
  virtual bool RegisterCallback(DiagDataCallback cb, void* context,
                                 std::string* error) = 0;

  // Same caveat as RegisterCallback: diag_switch_logging is
  // PUBLIC_REFERENCE_CONFIRMED void-returning, so the real
  // implementation cannot detect failure either. Kept as `bool` for the
  // same fake-shim testability reason.
  virtual bool SwitchLogging(int mode, std::string* error) = 0;

  // diag_callback_send_data - THE ONE call in this shim with a
  // confirmed, checked failure signal (0 = success). `proc` here is the
  // real, separate DESTINATION processor argument - unrelated to
  // RegisterCallback's `context`.
  virtual bool SendData(int proc, const void* buf, int len,
                         std::string* error) = 0;
};

// Real, libdiag.so-backed implementation.
class DiagCallbackShim : public IDiagCallbackShim {
 public:
  DiagCallbackShim() = default;
  ~DiagCallbackShim() override;

  DiagCallbackShim(const DiagCallbackShim&) = delete;
  DiagCallbackShim& operator=(const DiagCallbackShim&) = delete;

  bool Init(std::string* error) override;
  void Deinit() override;
  bool RegisterCallback(DiagDataCallback cb, void* context,
                         std::string* error) override;
  bool SwitchLogging(int mode, std::string* error) override;
  bool SendData(int proc, const void* buf, int len,
                std::string* error) override;
 private:
  bool owns_session_ = false;

};

}  // namespace guacamole_efs
