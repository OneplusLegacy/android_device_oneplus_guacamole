#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "diag_callback_shim.h"
#include "diag_frame.h"

namespace guacamole_efs {

// Abstract so higher-level logic can be exercised host-side against a
// fake, mirroring guacamole_pdc::QcciShim / IPdcClient's pattern.
class IDiagTransport {
 public:
  virtual ~IDiagTransport() = default;

  // Calls Diag_LSM_Init(nullptr) (via the injected IDiagCallbackShim).
  // Returns true and leaves `*error` untouched on success; returns
  // false and fills `*error` on failure. Sends NO subsystem packets -
  // unchanged behavior from the real-device-validated `transport-info`
  // command.
  virtual bool Init(std::string* error) = 0;

  // Calls Diag_LSM_DeInit(). Safe to call even if Init() was never
  // called or failed (no-ops in that case). Also cancels any pending
  // SendSubsysCommand() waiter - see DiagTransport::Deinit().
  virtual void Deinit() = 0;

  virtual bool initialized() const = 0;
};

enum class TransportStatus {
  kOk,
  kInitFailed,
  kRegisterCallbackFailed,
  kSwitchLoggingFailed,
  kSendFailed,
  kTimeout,
  kAlreadyOutstanding,     // MAX_OUTSTANDING_REQUESTS=1 violation - the
                           // shim was never touched for this call.
  kCancelled,              // Deinit() was called while this request was
                           // still outstanding.
  kInternalStateError,     // Fail-closed default: the post-wait state
                           // was something other than kCompleted/
                           // kCancelled. Should never happen given
                           // request_in_flight_'s atomicity - if it
                           // ever does, treat it as a bug, not a
                           // silent success.
  kSessionDesynchronized,  // A PRIOR request on this DiagTransport
                           // timed out; this transport-level session
                           // has no transaction token, so a late
                           // response to that old request could
                           // otherwise be mistaken for a new one's -
                           // every SendSubsysCommand() call is rejected
                           // until Deinit()+Init() start a fresh
                           // session. The shim was never touched for
                           // this call.
  kFrameIntegrityFailed,   // A correlated callback DID arrive (right
                           // header, right timing), but H.41's HDLC-
                           // style trailing CRC-16/X-25 did not match
                           // its own logical payload - see
                           // diag_frame.h. A genuine transport
                           // integrity failure, not an application-
                           // level EFS2 error - THIS request's own
                           // session is poisoned exactly like a
                           // timeout (see SendSubsysCommand()), no
                           // automatic retry.
};

const char* TransportStatusToString(TransportStatus status);

// Read-path outcome classification retained for existing diagnostics. Mutation
// callers MUST use IsMutationOutcomeAmbiguous, which also includes send failure.
bool IsTransportOutcomeAmbiguous(TransportStatus status);

// A failed SendData return does not prove that no bytes reached the modem.
// Send failure poisons the transport in all modes; mutating callers treat it
// as ambiguous and must dispose of the session, never retry on that session.
bool IsMutationOutcomeAmbiguous(TransportStatus status);

struct TransportResult {
  TransportStatus status = TransportStatus::kInitFailed;
  // NORMALIZED ("logical") response - the de-framed bytes every EFS2/
  // loopback parser actually consumes. Only meaningful when
  // status == kOk. Named `response` (not renamed) so every existing
  // Parse*/StripEfsResponseHeader caller needed zero changes when
  // framing normalization was added - it now transparently carries
  // de-framed bytes instead of raw ones. See diag_frame.h.
  std::vector<uint8_t> response;
  // Exactly what the callback delivered, before any de-framing -
  // populated whenever a correlated callback arrived at all (status is
  // kOk OR kFrameIntegrityFailed), empty otherwise. Preserved for
  // post-mortem analysis (see EfsWireCallLog / the CLI's
  // raw_response_hex output) - never fed to any EFS2 parser directly.
  std::vector<uint8_t> raw_response;
  // Framing/CRC metadata - see diag_frame.h's DiagFrameResult, which
  // these fields mirror exactly. Only meaningful alongside a populated
  // raw_response (status kOk or kFrameIntegrityFailed).
  bool had_hdlc_trailer = false;
  bool crc_valid = false;
  uint16_t crc_expected = 0;
  uint16_t crc_received = 0;
  // HDLC byte-stuffing metadata (2026-09-23 real-device finding - see
  // diag_frame.h) - only meaningful alongside a populated raw_response.
  bool escape_present = false;
  int escape_count = 0;
  size_t unescaped_len = 0;
};

// Real transport. Takes an IDiagCallbackShim so DiagTransport's OWN
// correlation/timeout state machine - not just orchestration built on
// top of it - is host-testable against a fake (tests/diag_transport_test.cpp),
// per the explicit Phase D0.5 requirement. Production code (main.cpp)
// constructs this with a real DiagCallbackShim.
class DiagTransport : public IDiagTransport {
 public:
  explicit DiagTransport(std::unique_ptr<IDiagCallbackShim> shim,
      DiagFrameMode frame_mode = DiagFrameMode::kH41Framed);
  ~DiagTransport() override;

  DiagTransport(const DiagTransport&) = delete;
  DiagTransport& operator=(const DiagTransport&) = delete;

  bool Init(std::string* error) override;
  void Deinit() override;
  bool initialized() const override;

  // One outstanding request per instance. io_mutex serializes all synchronous
  // shim calls with Deinit; the response wait is cancellable. Init refuses while
  // an earlier Send call has not returned. Missing H.41 trailer/CRC and send
  // failure/timeout poison the session. Never automatically retries.
  TransportResult SendSubsysCommand(int proc,
                                     const std::vector<uint8_t>& request,
                                     std::chrono::milliseconds timeout);

 private:
  enum class RequestState { kIdle, kWaiting, kCompleted, kTimedOut, kCancelled };

  static void CallbackTrampoline(void* buf, int len, void* context);
  void OnCallback(const uint8_t* buf, int len);

  // io_mutex serializes Init/Deinit and synchronous shim setup/send. It is
  // released before waiting, so shutdown can cancel a pending request. Callback
  // code takes only mutex_; no locks are held across callback thread join that
  // the callback itself needs. Owners must outlive all Send callers.
  std::mutex io_mutex_;
  const DiagFrameMode frame_mode_;
  mutable std::mutex lifecycle_mutex_;
  bool initialized_ = false;
  bool callback_ready_ = false;

  enum class SessionState { kReady, kDesynchronized };
  SessionState session_state_ = SessionState::kReady;

  std::atomic<bool> request_in_flight_{false};

  std::unique_ptr<IDiagCallbackShim> shim_;

  std::mutex mutex_;
  std::condition_variable cv_;
  RequestState state_ = RequestState::kIdle;
  std::vector<uint8_t> result_bytes_;

  // Response correlation key, valid only while has_expected_header_ is
  // true. Set from the current request's own first 4 bytes whenever it
  // looks like a subsys command (size>=4, request[0]==0x4B) - see this
  // file's header comment. Guarded by mutex_, same as state_/
  // result_bytes_, and cleared on every exit from a request (send
  // failure, completion, timeout, or cancellation) so a late callback
  // can never be matched against a NEWER, unrelated request by
  // coincidence.
  bool has_expected_header_ = false;
  uint8_t expected_header_[4] = {0, 0, 0, 0};

  // Defensive cap on a callback's claimed length - this project's own
  // trust boundary (not a DIAG spec value), same philosophy as
  // efs_wire.h's kMaxReadRequestBytes.
  static constexpr int kMaxCallbackBytes = 8192;
};

}  // namespace guacamole_efs
