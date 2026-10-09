#include "diag_transport.h"

#include "diag_frame.h"

namespace guacamole_efs {

const char* TransportStatusToString(TransportStatus status) {
  switch (status) {
    case TransportStatus::kOk:
      return "OK";
    case TransportStatus::kInitFailed:
      return "INIT_FAILED";
    case TransportStatus::kRegisterCallbackFailed:
      return "REGISTER_CALLBACK_FAILED";
    case TransportStatus::kSwitchLoggingFailed:
      return "SWITCH_LOGGING_FAILED";
    case TransportStatus::kSendFailed:
      return "SEND_FAILED";
    case TransportStatus::kTimeout:
      return "TIMEOUT";
    case TransportStatus::kAlreadyOutstanding:
      return "ALREADY_OUTSTANDING";
    case TransportStatus::kCancelled:
      return "CANCELLED";
    case TransportStatus::kInternalStateError:
      return "INTERNAL_STATE_ERROR";
    case TransportStatus::kSessionDesynchronized:
      return "SESSION_DESYNCHRONIZED";
    case TransportStatus::kFrameIntegrityFailed:
      return "FRAME_INTEGRITY_FAILED";
  }
  return "UNKNOWN";
}

bool IsTransportOutcomeAmbiguous(TransportStatus status) {
  switch (status) {
    case TransportStatus::kTimeout:
    case TransportStatus::kCancelled:
    case TransportStatus::kFrameIntegrityFailed:
    case TransportStatus::kInternalStateError:
      return true;
    case TransportStatus::kOk:
    case TransportStatus::kInitFailed:
    case TransportStatus::kRegisterCallbackFailed:
    case TransportStatus::kSwitchLoggingFailed:
    case TransportStatus::kSendFailed:
    case TransportStatus::kAlreadyOutstanding:
    case TransportStatus::kSessionDesynchronized:
      return false;
  }
  return true;  // fail-safe: an unrecognized status is treated as
                // ambiguous, never silently assumed safe.
}

bool IsMutationOutcomeAmbiguous(TransportStatus status) {
  // See this function's header comment (diag_transport.h): kSendFailed
  // is folded in as ambiguous ON TOP OF IsTransportOutcomeAmbiguous()'s
  // read-path classification, specifically for mutating requests, since
  // "the request never reached the modem" is not yet proven for this
  // failure mode.
  return IsTransportOutcomeAmbiguous(status) ||
         status == TransportStatus::kSendFailed;
}

DiagTransport::DiagTransport(std::unique_ptr<IDiagCallbackShim> shim,
    DiagFrameMode mode) : frame_mode_(mode), shim_(std::move(shim)) {}

DiagTransport::~DiagTransport() {
  bool was_initialized;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    was_initialized = initialized_;
  }
  if (was_initialized) {
    Deinit();
  }
}

bool DiagTransport::initialized() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  return initialized_;
}

bool DiagTransport::Init(std::string* error) {
  std::lock_guard<std::mutex> io_lock(io_mutex_);
  if (request_in_flight_.load()) {
    if (error) *error = "prior request has not returned";
    return false;
  }
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (initialized_) {
    if (error) *error = "DiagTransport::Init called twice without Deinit";
    return false;
  }
  // Diag_LSM_Init doesn't fire any data callback, so holding
  // lifecycle_mutex_ across it can't deadlock against OnCallback (which
  // never touches this lock) - see diag_transport.h's lock-role comment.
  if (!shim_->Init(error)) {
    return false;
  }
  initialized_ = true;
  session_state_ = SessionState::kReady;  // fresh session.
  return true;
}

void DiagTransport::Deinit() {
  // Wait for any synchronous Register/Switch/Send call to leave libdiag before
  // entering DeInit. Never switch logging to invalid mode 0.
  std::lock_guard<std::mutex> io_lock(io_mutex_);
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!initialized_) return;
    initialized_ = false;
    callback_ready_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RequestState::kWaiting) {
      state_ = RequestState::kCancelled;
      has_expected_header_ = false;
      cv_.notify_all();
    }
  }
  // H.41's DeInit joins its callback thread before returning. Keep callback
  // context alive until this finishes; no callback mutex is held here.
  shim_->Deinit();
}

TransportResult DiagTransport::SendSubsysCommand(
    int proc, const std::vector<uint8_t>& request,
    std::chrono::milliseconds timeout) {
  TransportResult result;

  // MAX_OUTSTANDING_REQUESTS=1, enforced atomically: "is a request
  // already outstanding" and "reserve the slot for this one" are the
  // SAME indivisible compare_exchange_strong operation, not two
  // separate steps a second thread could interleave between. See
  // diag_transport.h's lock-role comment for why this is a plain atomic
  // rather than a std::mutex.
  bool expected_not_in_flight = false;
  if (!request_in_flight_.compare_exchange_strong(expected_not_in_flight,
                                                    true)) {
    result.status = TransportStatus::kAlreadyOutstanding;
    return result;
  }
  // Released on every exit path via RAII, no matter which `return` runs.
  struct FlightGuard {
    std::atomic<bool>* flag;
    ~FlightGuard() { flag->store(false, std::memory_order_release); }
  } flight_guard{&request_in_flight_};

  std::unique_lock<std::mutex> io_lock(io_mutex_);

  bool is_initialized;
  bool desynchronized;
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    is_initialized = initialized_;
    desynchronized = (session_state_ == SessionState::kDesynchronized);
  }
  if (!is_initialized) {
    result.status = TransportStatus::kInitFailed;
    return result;
  }
  if (desynchronized) {
    // A prior request on THIS transport timed out. No transaction token
    // exists at this protocol layer, so a late response to that old
    // request could otherwise be mistaken for this new one's answer if
    // it happens to share the same subsys/cmd header. Refuse until the
    // caller explicitly restarts the session - no automatic recovery.
    result.status = TransportStatus::kSessionDesynchronized;
    return result;
  }

  std::string error;
  bool ready;
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    ready = callback_ready_;
  }
  if (!ready) {
    // `this` is registered as the callback's context - a genuinely
    // long-lived pointer (this object's own lifetime), never a local
    // variable's address. See diag_callback_shim.h for why the
    // previous version of this code (registering the address of a
    // local) was a dangling-pointer bug.
    if (!shim_->RegisterCallback(&DiagTransport::CallbackTrampoline, this,
                                  &error)) {
      result.status = TransportStatus::kRegisterCallbackFailed;
      return result;
    }
    if (!shim_->SwitchLogging(kDiagCallbackMode, &error)) {
      result.status = TransportStatus::kSwitchLoggingFailed;
      return result;
    }
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    callback_ready_ = true;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = RequestState::kWaiting;
    result_bytes_.clear();
    // Response correlation key: only set up for requests that actually
    // look like a subsys command (this function's real, documented
    // purpose - see diag_transport.h's header comment). A shorter or
    // non-0x4B-prefixed request gets no correlation guarantee; nothing
    // in this codebase's real usage sends such a request.
    if (request.size() >= 4 && request[0] == 0x4B) {
      has_expected_header_ = true;
      expected_header_[0] = request[0];
      expected_header_[1] = request[1];
      expected_header_[2] = request[2];
      expected_header_[3] = request[3];
    } else {
      has_expected_header_ = false;
    }
  }

  if (!shim_->SendData(proc, request.data(),
                        static_cast<int>(request.size()), &error)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_ = RequestState::kIdle;
      has_expected_header_ = false;
    }
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      session_state_ = SessionState::kDesynchronized;
    }
    result.status = TransportStatus::kSendFailed;
    return result;
  }
  io_lock.unlock();

  std::unique_lock<std::mutex> lock(mutex_);
  bool woke = cv_.wait_for(lock, timeout,
                            [this] { return state_ != RequestState::kWaiting; });
  if (!woke) {
    // Real timeout: close the request so any later callback sees a
    // state other than kWaiting and is discarded as late. No automatic
    // resend/retry - the caller decides what to do next. Also poisons
    // the session (see diag_transport.h's header comment) - this
    // transport has no transaction token, so a late response to THIS
    // exact request could be mistaken for a future one's if we allowed
    // sending again.
    state_ = RequestState::kTimedOut;
    has_expected_header_ = false;
    result.status = TransportStatus::kTimeout;
    lock.unlock();
    {
      std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
      session_state_ = SessionState::kDesynchronized;
    }
    return result;
  }

  // Exhaustive dispatch - NOT "anything other than kCancelled must be
  // kCompleted" (a bug this project already fixed once). request_in_flight_
  // makes kIdle/kWaiting unreachable here in practice; kInternalStateError
  // exists so a violation of that invariant fails closed instead of
  // silently reporting kOk.
  bool completed = false;
  std::vector<uint8_t> raw;
  switch (state_) {
    case RequestState::kCompleted:
      completed = true;
      raw = std::move(result_bytes_);
      state_ = RequestState::kIdle;
      has_expected_header_ = false;
      break;
    case RequestState::kCancelled:
      result.status = TransportStatus::kCancelled;
      has_expected_header_ = false;
      break;
    case RequestState::kTimedOut:
      result.status = TransportStatus::kTimeout;
      has_expected_header_ = false;
      break;
    case RequestState::kIdle:
    case RequestState::kWaiting:
      result.status = TransportStatus::kInternalStateError;
      has_expected_header_ = false;
      break;
  }
  if (!completed) {
    return result;
  }
  // Never hold mutex_ while touching lifecycle_mutex_ (see this file's
  // header comment on lock roles) - unlock before any possible
  // session-poisoning below.
  lock.unlock();

  // H41_DEVICE_CONFIRMED framing normalization (2026-09-23 preflight
  // finding, see diag_frame.h): a real callback response is NOT the
  // bare logical packet - it carries a trailing HDLC-style CRC-16/X-25
  // + 0x7e terminator that must be stripped here, ONCE, centrally -
  // never as a per-EFS2-parser special case.
  DiagFrameResult frame = NormalizeDiagFrame(raw, frame_mode_);
  result.raw_response = raw;
  result.had_hdlc_trailer = frame.had_hdlc_trailer;
  result.crc_valid = frame.crc_valid;
  result.crc_expected = frame.crc_expected;
  result.crc_received = frame.crc_received;
  result.escape_present = frame.escape_present;
  result.escape_count = frame.escape_count;
  result.unescaped_len = frame.unescaped_len;
  if (!frame.ok) {
    // A framed-but-CRC-mismatched (or framed-but-too-short) response is
    // a genuine transport INTEGRITY failure - not an application-level
    // EFS2 error - so it gets the exact same treatment as a timeout:
    // poison the session (no transaction token exists at this layer to
    // otherwise tell a stale/corrupted arrival apart from a future
    // request's real answer), no automatic retry.
    result.status = TransportStatus::kFrameIntegrityFailed;
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    session_state_ = SessionState::kDesynchronized;
    return result;
  }
  result.status = TransportStatus::kOk;
  result.response = std::move(frame.logical);
  return result;
}

void DiagTransport::CallbackTrampoline(void* buf, int len, void* context) {
  auto* self = static_cast<DiagTransport*>(context);
  if (self != nullptr) {
    self->OnCallback(static_cast<const uint8_t*>(buf), len);
  }
}

void DiagTransport::OnCallback(const uint8_t* buf, int len) {
  // Deliberately minimal work here - no parsing, no blocking. Just
  // validate, correlate, copy, and notify; efs_wire.h does all real
  // parsing later, outside this callback's call stack entirely.
  //
  // CORRELATION ORDERING (clarified 2026-09-23, per the same review
  // that found HDLC byte-stuffing): the structurally correct pipeline
  // is raw callback -> HDLC unescape/CRC-verify (diag_frame.h) ->
  // logical packet -> header correlation -> command parser. The header
  // check below deliberately runs on the RAW bytes instead, BEFORE
  // normalization (which only happens later, in SendSubsysCommand(),
  // for whichever single callback actually gets accepted here) - this
  // is safe ONLY because this codebase's entire fixed command set uses
  // header bytes (0x4B magic; subsys 0x12/0x13; cmd codes 2/3/4/11/12/
  // 13/15/0x29) that can never coincide with the HDLC escape byte
  // (0x7D) or terminator (0x7E), so raw-byte stuffing never shifts or
  // corrupts these specific 4 header bytes in practice - confirmed by
  // all 3 real responses seen so far, none of which needed the header
  // itself escaped. Doing the cheap raw check here (before the more
  // expensive full unescape+CRC) also avoids normalizing every
  // unrelated/unsolicited callback that arrives while a request is
  // outstanding. If this project ever adds a command whose header byte
  // COULD collide with 0x7D/0x7E, this ordering would need revisiting.
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != RequestState::kWaiting) {
    // Unsolicited (kIdle), late (kTimedOut/kCancelled), or a duplicate
    // arriving after a request already completed (kCompleted) -
    // discarded, does not touch result_bytes_ or complete anything.
    return;
  }
  if (buf == nullptr) return;
  if (len <= 0) return;
  if (len > kMaxCallbackBytes) return;

  if (has_expected_header_) {
    // Response correlation: this callback must echo the exact 4-byte
    // header of the request it's supposedly answering. Anything else -
    // too short, or a mismatched magic/subsys/cmd - is UNRELATED DIAG
    // data that happens to arrive while this request is outstanding.
    // Discarded WITHOUT touching state_/the deadline: the request stays
    // kWaiting and keeps counting down from its own original send time,
    // not from this unrelated arrival.
    if (len < 4) return;
    if (buf[0] != expected_header_[0] || buf[1] != expected_header_[1] ||
        buf[2] != expected_header_[2] || buf[3] != expected_header_[3]) {
      return;
    }
  }

  // Copy BEFORE returning - the caller (real libdiag, or a test's fake
  // shim) may reuse/free `buf` the instant this function returns.
  result_bytes_.assign(buf, buf + len);
  state_ = RequestState::kCompleted;
  has_expected_header_ = false;
  cv_.notify_one();
}

}  // namespace guacamole_efs
