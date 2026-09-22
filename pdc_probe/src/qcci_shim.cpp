#include "qcci_shim.h"

// extern "C" prototypes for symbols this probe links against from
// libqmi_cci.so / libqmiservices.so (see qcci_shim.h's file header for
// full provenance). Qualcomm does not publish these headers, so these
// declarations are hand-written to match the real exported symbol names
// (confirmed via `readelf -Ws` against this firmware) and the parameter
// shapes documented/cross-checked there. As with any hand-written
// extern "C" declaration for a third-party prebuilt library, an
// incorrect shape here would be a silent ABI mismatch rather than a
// build error - this is the one place in this codebase where that risk
// genuinely cannot be fully eliminated without Qualcomm's own headers.
extern "C" {
void* pdc_get_service_object_internal_v01(int32_t major, int32_t minor,
                                           int32_t tool);
int qmi_client_init_instance(guacamole_pdc::QmiServiceObject service_obj,
                              unsigned int instance_id,
                              guacamole_pdc::QmiIndicationCallback ind_cb,
                              void* ind_cb_data, void* os_params,
                              uint32_t timeout_ms,
                              guacamole_pdc::QmiClientHandle* out_handle);
int qmi_client_release(guacamole_pdc::QmiClientHandle handle);
int qmi_client_send_raw_msg_sync(guacamole_pdc::QmiClientHandle handle,
                                  unsigned int msg_id, void* req_buf,
                                  unsigned int req_len, void* resp_buf,
                                  unsigned int resp_buf_capacity,
                                  unsigned int* resp_recv_len,
                                  unsigned int timeout_ms);
}

namespace guacamole_pdc {

bool QcciShim::Load(std::string* error) {
  // Nothing to dynamically load: both libraries are linked at build
  // time via Android.bp `shared_libs`. This just marks the wrapper
  // ready and gives a single place to extend if a future revision needs
  // an actual runtime check.
  (void)error;
  loaded_ = true;
  return true;
}

QmiServiceObject QcciShim::PdcGetServiceObject(int32_t major, int32_t minor,
                                                int32_t tool) {
  if (!loaded_) return nullptr;
  return pdc_get_service_object_internal_v01(major, minor, tool);
}

int QcciShim::ClientInitInstance(QmiServiceObject service_obj,
                                  QmiIndicationCallback ind_cb,
                                  void* ind_cb_data, uint32_t timeout_ms,
                                  QmiClientHandle* out_handle) {
  if (!loaded_) return -1;
  // instance=kQmiClientInstanceAny, os_params=NULL: matches
  // missing_qmi_client_api.h's already-working usage of this same
  // function (from this same library) for the DMS service.
  return qmi_client_init_instance(service_obj, kQmiClientInstanceAny, ind_cb,
                                   ind_cb_data, nullptr, timeout_ms,
                                   out_handle);
}

int QcciShim::ClientRelease(QmiClientHandle handle) {
  if (!loaded_ || handle == nullptr) return -1;
  return qmi_client_release(handle);
}

int QcciShim::SendRawMsgSync(QmiClientHandle handle, unsigned int msg_id,
                              const void* req_buf, unsigned int req_len,
                              void* resp_buf, unsigned int resp_buf_capacity,
                              unsigned int* resp_recv_len,
                              unsigned int timeout_ms) {
  if (!loaded_ || handle == nullptr) return -1;
  return qmi_client_send_raw_msg_sync(
      handle, msg_id, const_cast<void*>(req_buf), req_len, resp_buf,
      resp_buf_capacity, resp_recv_len, timeout_ms);
}

}  // namespace guacamole_pdc
