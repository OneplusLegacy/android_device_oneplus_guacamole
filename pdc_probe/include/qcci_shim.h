// guacamole_pdc_probe - thin wrapper around Qualcomm's QCCI client
// library and the PDC service object accessor.
//
// LINKING, NOT dlopen: both libraries this probe needs already have real
// `cc_prebuilt_library_shared` Android.bp modules in this exact tree
// (`vendor/oneplus/sm8150-common/Android.bp`: `libqmi_cci` at line 2051,
// `libqmiservices` at line 2362 as of this writing) - unlike
// `libqcci_legacy.so`, which has NO module at all (see the REVISION
// NOTE below for why that library was dropped from this design). Since
// proper build-time modules exist, this probe links against them via
// ordinary `shared_libs:` in Android.bp and declares the handful of
// `extern "C"` prototypes it needs itself (Qualcomm does not publish
// the headers), instead of dlopen()/dlsym(). This is strictly more
// robust than dlopen: a missing/renamed symbol becomes a LINK failure
// caught at build time, not a runtime dlsym() error discovered only on
// a real device.
//
// REVISION NOTE (history, kept for context): this file originally
// dlopen()ed `libqcci_legacy.so` and called `qmi_client_init(NULL
// service_info, ...)`, on the unverified hypothesis that NULL triggers
// automatic service discovery. A code review flagged that assumption as
// the highest-risk unverified piece of this probe. Direct
// re-verification found:
//   1. `libqcci_legacy.so` has NO service-discovery function at all
//      (`readelf -Ws`, both 32/64-bit builds: only
//      qmi_client_init/release/send_(raw_)msg_(a)sync/message_encode|
//      decode/get_service_version/delete_async_txn).
//   2. `libqmi_cci.so` - a separate, physically distinct library also
//      on this device - is a strictly more complete implementation: it
//      exports everything libqcci_legacy.so does, PLUS
//      `qmi_client_get_service_list`, `qmi_client_get_any_service`,
//      `qmi_client_get_service_instance`, `qmi_client_init_instance`,
//      `qmi_client_notifier_init`, and more (confirmed via `readelf -Ws`
//      on both 32/64-bit builds - 19 real, defined, GLOBAL/DEFAULT
//      function symbols).
// A radare2 disassembly of `imsqmidaemon` (a real, already-shipping,
// independent QMI client on this exact firmware, linking libqmi_cci.so)
// shows it calling `qmi_client_get_service_list()` and passing THAT
// buffer as `qmi_client_init`'s first argument - i.e. NULL is
// definitively NOT how a real client on this device does this.
//
// This shim uses `qmi_client_init_instance()` instead of raw
// `qmi_client_init` + a hand-decoded `qmi_client_get_service_list()` -
// a one-shot convenience function that resolves-and-connects in a
// single call. This exact function, from this exact library, already
// has a proven precedent in this PixelOS tree:
// hardware/qcom-caf/wlan/wcnss-service/missing_qmi_client_api.h declares
// and dlsym's `qmi_client_init_instance` from `libqmi_cci_handle` for
// the DMS service - same library, same function, different QMI service
// (that file used dlopen for a library WITHOUT a bp module at the time
// it was written for DMS's other dependency; here we have real modules
// for both libraries, so we link normally instead).
//
// SIGNATURE PROVENANCE (read this before changing anything):
//   - Confirmed BY NAME in this exact firmware (`readelf -Ws` on
//     vendor/lib{,64}/libqmi_cci.so and libqmiservices.so):
//       qmi_client_init_instance, qmi_client_release,
//       qmi_client_send_raw_msg_sync, pdc_get_service_object_internal_v01
//   - Parameter COUNT/ORDER for `qmi_client_init_instance` is taken
//     directly from `missing_qmi_client_api.h`'s already-working
//     declaration in this tree:
//       int (*qmi_client_init_instance)(void*, unsigned int, void*,
//                                        void**, void**, uint32_t, void**);
//     i.e. (service_obj, instance_id, ind_cb, ind_cb_data, os_params,
//     timeout_msecs, &user_handle) - 7 arguments. The exact pointer
//     sub-types in that declaration (void* vs void**) don't affect
//     calling-convention correctness here: on this AAPCS64 target every
//     slot in that list is a single 8-byte register-passed value
//     (pointer or promoted integer), so what matters is the count,
//     order, and pointer-vs-32-bit-integer class of each slot, not C's
//     static type distinctions within "pointer-sized" - the extern "C"
//     declaration below preserves that mapping exactly, using more
//     precise types for readability.
//     `QMI_CLIENT_INSTANCE_ANY = 0xffff` (from the same header) is used
//     for instance_id: this device has exactly one local PDC instance.
//   - `qmi_client_send_raw_msg_sync`'s shape is additionally cross-
//     checked against Qualcomm's public qmi-framework repo
//     (`qmi_cci_send_raw_msg_sync` in include/qmi_cci.h), which
//     documents the same parameter shape for the modern renamed
//     equivalent of this API.
//   - The (major, minor, tool) version triple `pdc_get_service_object_internal_v01`
//     needs is CONFIRMED as (1, 15, 6) via a radare2 cross-reference pass
//     against this exact H.41 libril-qc-hal-qmi.so - two independent call
//     sites (`pdc_init()` and `PdcModemEndPointModule::getServiceObject()`)
//     both load these exact literals. See
//     research_pdc_service_object_version.md. `main.cpp` uses ONLY this
//     value (no fallback list) - see that file's comment for why.
//   - NOT byte-verified against THIS specific call (`qmi_client_init_instance`
//     for the PDC service, as opposed to DMS): the general shape is
//     proven-in-tree for a different service on the same library, and
//     `qmi_client_init`'s real 6-argument shape (service_info, service_obj,
//     ind_cb, ind_cb_data, os_params, &handle) was independently
//     disassembly-confirmed at imsqmidaemon's call site - both are
//     strong, but neither is a byte-exact confirmation of
//     `qmi_client_init_instance` specifically. Treat the first on-device
//     run of this probe as validating this assumption, not as a routine
//     test run.

#pragma once

#include <cstdint>
#include <string>

namespace guacamole_pdc {

using QmiClientHandle = void*;
using QmiServiceObject = void*;

// Matches qmi_client_ind_cb from Qualcomm's public qmi_cci.h:
//   void (qmi_client_type user_handle, unsigned int msg_id,
//         void *ind_buf, unsigned int ind_buf_len, void *ind_cb_data)
using QmiIndicationCallback = void (*)(QmiClientHandle, unsigned int, void*,
                                        unsigned int, void*);

constexpr int kQmiNoErr = 0;  // QMI_NO_ERR, confirmed convention (0 = success)
                              // across every public Qualcomm QMI header seen
                              // in this research (qmi_cci.h, missing_qmi_client_api.h).

// QMI_CLIENT_INSTANCE_ANY, from missing_qmi_client_api.h (already-working
// in-tree precedent) - connect to whichever single instance of the
// service the modem exposes, which is what a locally-embedded modem's
// PDC service is expected to be.
constexpr unsigned int kQmiClientInstanceAny = 0xffff;

// Sole, CONFIRMED (major, minor, tool) triple for
// pdc_get_service_object_internal_v01 - see research_pdc_service_object_version.md.
// No fallback list: see main.cpp's comment for why failing closed here
// is the right call for a single-device-targeted tool.
struct VersionTriple {
  int32_t major;
  int32_t minor;
  int32_t tool;
};

// Thin, directly-linked wrapper - `Load()` is kept only so callers have
// one uniform place to check "is the transport ready" and one uniform
// error-reporting shape, even though there is no dynamic loading left
// to do (symbol resolution now happens at link time, not here).
class QcciShim {
 public:
  QcciShim() = default;
  ~QcciShim() = default;

  QcciShim(const QcciShim&) = delete;
  QcciShim& operator=(const QcciShim&) = delete;

  bool Load(std::string* error);

  // Wrappers around the linked functions. All return kQmiNoErr (0) on
  // success; any other value is treated as failure by callers.
  QmiServiceObject PdcGetServiceObject(int32_t major, int32_t minor,
                                       int32_t tool);
  int ClientInitInstance(QmiServiceObject service_obj,
                          QmiIndicationCallback ind_cb, void* ind_cb_data,
                          uint32_t timeout_ms, QmiClientHandle* out_handle);
  int ClientRelease(QmiClientHandle handle);
  int SendRawMsgSync(QmiClientHandle handle, unsigned int msg_id,
                      const void* req_buf, unsigned int req_len,
                      void* resp_buf, unsigned int resp_buf_capacity,
                      unsigned int* resp_recv_len, unsigned int timeout_ms);

  bool loaded() const { return loaded_; }

 private:
  bool loaded_ = false;
};

}  // namespace guacamole_pdc
