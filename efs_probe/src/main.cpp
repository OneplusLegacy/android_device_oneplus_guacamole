// guacamole_efs_probe - CLI entry point.
//
// `transport-info`: H41_DEVICE_CONFIRMED PASS (Diag_LSM_Init/DeInit
// only, sends NO EFS2 or any other subsystem packets).
//
// `callback-smoke`: H41_DEVICE_CONFIRMED PASS (2026-09-23) - a full
// callback-mode request/response round trip with the real modem,
// header-correlated, `header_match=true`. This is the first real
// evidence `DiagTransport::SendSubsysCommand()` genuinely works
// end-to-end. Uses only `include/diag_loopback_packet.h`'s byte-exact,
// non-EFS2 H.41 payload - never sends an EFS2 packet. See
// diag_transport.h's header comment for the full result and for a bug
// this run found and fixed (Deinit() no longer calls SwitchLogging(0) -
// H.41 rejects mode 0 as invalid).
//
// `stat-mcfg`: the first real EFS2 operation - a single STAT against
// the fixed `/nv/item_files/mcfg` target (never a caller-supplied
// path). Implemented and host-tested
// (tests/stat_mcfg_test.cpp) but NOT yet run on a real device - that
// is a separate action requiring explicit go-ahead, same as every
// other real-device step in this project. `EFS2_SUBSYSTEM_ID` (`0x13`)
// itself is `QUALCOMM_PROTOCOL_CONFIRMED` but still
// `H41_DEVICE_NOT_YET_CONFIRMED` - running this command for real is
// exactly what would confirm or refute it.
//
// Every other subcommand name prints "not implemented this phase" and
// performs no DIAG I/O at all - see PHASE_D1_EFS2_READONLY_FINDINGS.md
// for the full status. When they DO get implemented, each one maps to
// exactly one fixed EfsTarget from efs_targets.h - none of them will
// ever accept a caller-supplied path string, matching `stat-mcfg`'s own
// design.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "callback_smoke.h"
#include "diag_callback_shim.h"
#include "diag_loopback_packet.h"
#include "diag_transport.h"
#include "diag_session_cycle.h"
#include "diag_process_worker.h"
#include "process_efs_executor.h"
#include "efs_real_client.h"
#include "efs_reader.h"
#include "efs_snapshot.h"
#include "efs_targets.h"
#include "hash.h"
#include "stat_mcfg.h"

namespace {

int RunTransportInfo() {
  guacamole_efs::DiagTransport transport(
      std::make_unique<guacamole_efs::DiagCallbackShim>());
  std::string error;
  std::printf("guacamole_efs_probe transport-info\n");
  std::printf("Calling Diag_LSM_Init(nullptr)...\n");
  if (!transport.Init(&error)) {
    std::printf("RESULT: FAILED - %s\n", error.c_str());
    return 1;
  }
  std::printf("RESULT: Diag_LSM_Init succeeded.\n");
  std::printf("Calling Diag_LSM_DeInit()...\n");
  transport.Deinit();
  std::printf("RESULT: OK - init/deinit round trip completed. No EFS2 or "
              "any other subsystem packets were sent.\n");
  return 0;
}

int RunNotImplemented(const std::string& cmd) {
  std::printf(
      "%s: NOT_IMPLEMENTED_THIS_PHASE\n"
      "The EFS2 request/response round trip is not wired up yet - see\n"
      "diag_transport.h and PHASE_D1_EFS2_READONLY_FINDINGS.md.\n"
      "No DIAG I/O was performed by this command.\n",
      cmd.c_str());
  return 2;
}

std::string ToHex(const std::vector<uint8_t>& bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(kDigits[(b >> 4) & 0xF]);
    out.push_back(kDigits[b & 0xF]);
  }
  return out;
}

int RunCallbackSmokeCommand() {
  using guacamole_efs::kH41LoopbackPacket;
  std::vector<uint8_t> request(kH41LoopbackPacket.begin(),
                                kH41LoopbackPacket.end());
  const auto timeout = std::chrono::milliseconds(2000);

  std::printf("request=%s\n", ToHex(request).c_str());
  std::printf("proc=0\n");
  std::printf("timeout_ms=%lld\n",
              static_cast<long long>(timeout.count()));

  guacamole_efs::DiagTransport transport(
      std::make_unique<guacamole_efs::DiagCallbackShim>());
  std::string error;
  if (!transport.Init(&error)) {
    std::printf("transport_status=INIT_FAILED\n");
    std::printf("error=%s\n", error.c_str());
    return 1;
  }

  // Exactly one command. No automatic retry - a timeout or failure is
  // reported as-is; this command does not decide to try again.
  auto result = guacamole_efs::RunCallbackSmoke(&transport, timeout);

  std::printf("transport_status=%s\n",
              guacamole_efs::TransportStatusToString(result.status));
  std::printf("response_len=%zu\n", result.response.size());
  std::printf("response_hex=%s\n", ToHex(result.response).c_str());

  bool header_match =
      result.response.size() >= 4 &&
      std::equal(kH41LoopbackPacket.begin(), kH41LoopbackPacket.begin() + 4,
                 result.response.begin());
  std::printf("header_match=%s\n", header_match ? "true" : "false");

  transport.Deinit();

  return result.status == guacamole_efs::TransportStatus::kOk ? 0 : 1;
}

int RunStatMcfgCommand() {
  using guacamole_efs::EfsTarget;
  const auto timeout = std::chrono::milliseconds(2000);

  guacamole_efs::DiagTransport transport(
      std::make_unique<guacamole_efs::DiagCallbackShim>());
  std::string error;

  std::printf("target=%s\n",
              guacamole_efs::TargetPath(EfsTarget::kMcfgDir).c_str());

  if (!transport.Init(&error)) {
    std::printf("transport_status=INIT_FAILED\n");
    std::printf("error=%s\n", error.c_str());
    return 1;
  }

  // Exactly one command. No automatic retry - a timeout or failure is
  // reported as-is; this command does not decide to try again.
  auto result = guacamole_efs::RunStatMcfg(&transport, timeout);

  std::printf("request_hex=%s\n", ToHex(result.request).c_str());
  std::printf("transport_status=%s\n",
              guacamole_efs::TransportStatusToString(result.transport.status));
  std::printf("response_len=%zu\n", result.transport.response.size());
  std::printf("response_hex=%s\n", ToHex(result.transport.response).c_str());

  bool header_match =
      result.request.size() >= 4 && result.transport.response.size() >= 4 &&
      std::equal(result.request.begin(), result.request.begin() + 4,
                 result.transport.response.begin());
  std::printf("header_match=%s\n", header_match ? "true" : "false");
  std::printf("\n");

  transport.Deinit();

  if (result.transport.status != guacamole_efs::TransportStatus::kOk) {
    std::printf("parse_status=NOT_ATTEMPTED\n");
    return 1;
  }
  if (!result.parsed) {
    std::printf("parse_status=MALFORMED\n");
    return 1;
  }

  // Transport success (a correctly-correlated response arrived) is NOT
  // the same as EFS success - the modem's own EFS layer can still
  // report an error via a nonzero diag_errno. Both are reported
  // separately; exit code reflects the EFS-level result, not just the
  // transport-level one.
  std::printf("parse_status=%s\n",
              result.stat.diag_errno == 0 ? "OK" : "EFS_ERROR");
  std::printf("diag_errno=%d\n", result.stat.diag_errno);
  std::printf("mode=%d\n", result.stat.mode);
  std::printf("size=%d\n", result.stat.size);
  std::printf("nlink=%d\n", result.stat.nlink);
  std::printf("atime=%d\n", result.stat.atime);
  std::printf("mtime=%d\n", result.stat.mtime);
  std::printf("ctime=%d\n", result.stat.ctime);

  return result.stat.diag_errno == 0 ? 0 : 1;
}

std::string Sha256Hex(const std::array<uint8_t, 32>& digest) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint8_t b : digest) {
    out.push_back(kDigits[(b >> 4) & 0xF]);
    out.push_back(kDigits[b & 0xF]);
  }
  return out;
}

// Recommended (not blocking) Cycle 1 preflight improvement: the 2 debug
// log targets can be arbitrarily large, and dumping their FULL
// data_hex to stdout could make a real-device run's output unwieldy.
// Every OTHER target's full raw bytes are always printed in full - see
// RunSnapshotMcfgReadonlyCommand. The full raw bytes are NEVER dropped
// from the in-memory EfsSnapshotEntry.data itself (sha256 is always
// computed over the complete data, not the preview) - this bounds only
// what this CLI command prints to stdout for these 2 targets.
bool IsDebugLogTarget(guacamole_efs::EfsTarget t) {
  return t == guacamole_efs::EfsTarget::kMcfgDebugLog ||
         t == guacamole_efs::EfsTarget::kMcfgDebugOldLog;
}
constexpr size_t kDebugLogHexPreviewBytes = 256;

const char* PresenceToString(guacamole_efs::TargetPresence p) {
  switch (p) {
    case guacamole_efs::TargetPresence::kExists:
      return "EXISTS";
    case guacamole_efs::TargetPresence::kAbsent:
      return "ABSENT";
    case guacamole_efs::TargetPresence::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

void PrintWireCallLog(const guacamole_efs::EfsWireCallLog& log) {
  std::printf("request_hex=%s\n", ToHex(log.request).c_str());
  std::printf("transport_status=%s\n",
              guacamole_efs::TransportStatusToString(log.transport_status));
  // Raw and logical are printed SEPARATELY (2026-09-23 preflight
  // finding - see diag_frame.h): a real H.41 callback response carries
  // a trailing HDLC-style CRC-16/X-25 + 0x7e terminator that
  // DiagTransport strips centrally before any EFS2 parser ever sees
  // the bytes. `raw_response_hex` is exactly what the callback
  // delivered; `logical_response_hex` is what every efs_status/
  // diag_errno line below actually reflects.
  std::printf("raw_response_len=%zu\n", log.raw_response.size());
  std::printf("raw_response_hex=%s\n", ToHex(log.raw_response).c_str());
  std::printf("hdlc_trailer_present=%s\n",
              log.had_hdlc_trailer ? "true" : "false");
  if (log.had_hdlc_trailer) {
    // HDLC byte-stuffing metadata (2026-09-23 real-device finding: a
    // real READDIR response's own CRC byte needed unescaping - see
    // diag_frame.h) - printed whenever framing was present at all, not
    // just when escaping actually occurred, so a post-mortem reader
    // can confirm escape_present=false was the actual observed case
    // for a given call rather than an unprinted assumption.
    std::printf("hdlc_escape_present=%s\n",
                log.escape_present ? "true" : "false");
    std::printf("hdlc_escape_count=%d\n", log.escape_count);
    std::printf("unescaped_frame_len=%zu\n", log.unescaped_len);
    std::printf("crc_expected=0x%04x\n", log.crc_expected);
    std::printf("crc_received=0x%04x\n", log.crc_received);
    std::printf("crc_valid=%s\n", log.crc_valid ? "true" : "false");
  }
  std::printf("logical_response_len=%zu\n", log.logical_response.size());
  std::printf("logical_response_hex=%s\n",
              ToHex(log.logical_response).c_str());
  bool header_match =
      log.request.size() >= 4 && log.logical_response.size() >= 4 &&
      std::equal(log.request.begin(), log.request.begin() + 4,
                 log.logical_response.begin());
  std::printf("header_match=%s\n", header_match ? "true" : "false");
  // CORRECTED 2026-09-23 preflight review: a transport-level failure
  // (no correlated response ever arrived - kTimeout, kCancelled, etc.)
  // means the payload parser was never even invoked, and must NOT be
  // reported as "MALFORMED" - that word specifically means "a response
  // arrived but the parser rejected it", i.e. a payload-level defect.
  // Conflating the two hid exactly which layer produced a given
  // failure during post-mortem analysis.
  const char* efs_status;
  if (log.transport_status != guacamole_efs::TransportStatus::kOk) {
    efs_status = "NOT_PARSED";
  } else if (!log.parsed_ok) {
    efs_status = "MALFORMED";
  } else if (log.diag_errno != 0) {
    efs_status = "EFS_ERROR";
  } else {
    efs_status = "OK";
  }
  std::printf("efs_status=%s\n", efs_status);
  std::printf("diag_errno=%d\n", log.diag_errno);
}

// Runs the fixed, single-invocation 9-step read-only snapshot (see
// efs_snapshot.h) and prints rich per-step output for full post-mortem
// analysis from ONE real-device run - the exact format the Cycle 1
// review requires: a "=== STEP: <name> ===" header, every underlying
// wire round trip that step issued (a step can issue several - e.g. a
// directory listing is OPENDIR + N*READDIR + CLOSEDIR), then
// step-type-specific fields (STAT metadata / directory entries / file
// read result). Fail-fast: RunMcfgReadonlySnapshot() itself stops at
// the first triggering condition (see efs_snapshot.h) - this function
// does not retry or continue past what that call already decided, and
// performs no DIAG I/O of its own beyond what that one call performs.
int RunSnapshotMcfgReadonlyCommand() {
  const auto timeout = std::chrono::milliseconds(2000);

  std::printf("guacamole_efs_probe snapshot-mcfg-readonly\n");
  std::printf("muxd_read_support=1\n");

  guacamole_efs::DiagTransport transport(
      std::make_unique<guacamole_efs::DiagCallbackShim>());
  std::string error;
  if (!transport.Init(&error)) {
    std::printf("transport_status=INIT_FAILED\n");
    std::printf("error=%s\n", error.c_str());
    return 1;
  }

  guacamole_efs::EfsRealClient client(&transport, timeout);

  // One boundary index into client.CallLog() per step, recorded the
  // instant each step begins - lets the wire calls a step actually
  // issued be sliced out of the client's single chronological log
  // after the whole snapshot has run, since EfsSnapshotEntry itself
  // deliberately carries no raw wire bytes (see efs_real_client.h's
  // header comment on why).
  std::vector<size_t> step_call_log_start;
  auto hook = [&](const std::string&) {
    step_call_log_start.push_back(client.CallLog().size());
  };

  auto result = guacamole_efs::RunMcfgReadonlySnapshot(&client, hook);

  for (size_t i = 0; i < result.entries.size(); i++) {
    const auto& e = result.entries[i];
    std::printf("=== STEP: %s ===\n", e.step_name.c_str());
    std::printf("target=%s\n", e.discovered_path.empty()
        ? guacamole_efs::TargetPath(e.target).c_str() : e.discovered_path.c_str());
    if (!e.collection_status.empty()) {
      std::printf("collection_status=%s\n", e.collection_status.c_str());
      std::printf("listed_size=%d\n", e.listed_size);
      if (!e.discovered_name.empty())
        std::printf("name=%s\n", e.discovered_name.c_str());
    }

    size_t begin = step_call_log_start[i];
    size_t end = (i + 1 < step_call_log_start.size())
                     ? step_call_log_start[i + 1]
                     : client.CallLog().size();
    for (size_t j = begin; j < end; j++) {
      const auto& log = client.CallLog()[j];
      std::printf("--- wire: %s ---\n", log.label.c_str());
      PrintWireCallLog(log);
    }

    std::printf("presence=%s\n", PresenceToString(e.presence));
    // Only meaningful for the 4 targets that are direct children of
    // /nv/item_files/mcfg - always false (and printed as such) for the
    // 3 outside it, matching efs_snapshot.h's documented scope for
    // this field.
    std::printf("directory_entry_seen=%s\n",
                 e.directory_entry_seen ? "true" : "false");
    if (e.has_stat) {
      std::printf("mode=%d\n", e.stat_mode);
      std::printf("size=%d\n", e.stat_size);
      std::printf("nlink=%d\n", e.stat_nlink);
      std::printf("atime=%d\n", e.stat_atime);
      std::printf("mtime=%d\n", e.stat_mtime);
      std::printf("ctime=%d\n", e.stat_ctime);
    }
    if (e.is_directory) {
      if (e.has_dir_entries) {
        for (size_t k = 0; k < e.dir_entries.size(); k++) {
          const auto& d = e.dir_entries[k];
          std::printf("entry_index=%zu\n", k);
          std::printf("entry_name=%s\n", d.name.c_str());
          std::printf("entry_type=%d\n", d.entry_type);
          std::printf("entry_size=%d\n", d.size);
        }
      }
    } else {
      std::printf("expected_size=%d\n", e.stat_size);
      if (e.has_data) {
        std::printf("bytes_read=%zu\n", e.data.size());
      }
      if (e.has_sha256) {
        std::printf("sha256=%s\n", Sha256Hex(e.sha256).c_str());
        if (e.discovered_path.empty() && IsDebugLogTarget(e.target) &&
            e.data.size() > kDebugLogHexPreviewBytes) {
          std::vector<uint8_t> preview(
              e.data.begin(), e.data.begin() + kDebugLogHexPreviewBytes);
          std::printf("data_hex_truncated=true\n");
          std::printf("data_hex_preview_bytes=%zu\n",
                      kDebugLogHexPreviewBytes);
          std::printf("data_hex=%s\n", ToHex(preview).c_str());
        } else {
          std::printf("data_hex_truncated=false\n");
          std::printf("data_hex=%s\n", ToHex(e.data).c_str());
        }
      }
    }
    if (!e.message.empty()) {
      std::printf("message=%s\n", e.message.c_str());
    }
    std::printf("\n");
  }

  transport.Deinit();

  const char* outcome_str = "FAILED";
  switch (result.outcome) {
    case guacamole_efs::SnapshotOutcome::kDone:
      outcome_str = "DONE";
      break;
    case guacamole_efs::SnapshotOutcome::kDoneWithWarnings:
      outcome_str = "DONE_WITH_WARNINGS";
      break;
    case guacamole_efs::SnapshotOutcome::kFailed:
      outcome_str = "FAILED";
      break;
  }
  std::printf("outcome=%s\n", outcome_str);
  if (result.outcome == guacamole_efs::SnapshotOutcome::kFailed) {
    std::printf("fail_reason=%s\n", result.fail_reason.c_str());
  }

  // DONE_WITH_WARNINGS is still success for exit-code purposes - the
  // whole 9-step sequence completed; the warnings are about presence
  // ambiguity on individual optional targets, not a sequence failure.
  return result.outcome == guacamole_efs::SnapshotOutcome::kFailed ? 1 : 0;
}

// Internal exec-only worker: fixed target, no mode/path/config arguments.
int RunDiagReadWorker(const char* token) {
  using namespace guacamole_efs;
  WorkerNonce nonce{};
  struct stat pipe_stat{};
  const int flags = fcntl(kDiagWorkerResultFd, F_GETFL);
  const pid_t parent = getppid();
  if (!ParseWorkerNonce(token, &nonce) || flags < 0 ||
      (flags & O_ACCMODE) != O_WRONLY ||
      fstat(kDiagWorkerResultFd, &pipe_stat) != 0 || !S_ISFIFO(pipe_stat.st_mode) ||
      parent <= 1 || prctl(PR_SET_PDEATHSIG, static_cast<unsigned long>(SIGKILL), 0UL, 0UL, 0UL) != 0 || getppid() != parent) return 2;
  DiagEfsSessionFactory factory([] { return std::make_unique<DiagCallbackShim>(); });
  const auto phase = RunSingleDiagDigestRead(factory, [](const std::string& event) {
    std::fprintf(stderr, "diag_worker_event=%s\n", event.c_str());
  });
  WorkerReadReport report;
  report.pid = static_cast<uint32_t>(getpid());
  report.nonce = nonce;
  report.status = phase.status == "READ_OK" ? WorkerReadStatus::kOk :
      phase.status == "INIT_FAILED" ? WorkerReadStatus::kInitFailed : WorkerReadStatus::kReadFailed;
  report.teardown_completed = phase.teardown_completed;
  report.bytes = phase.bytes;
  report.sha256 = phase.sha256;
  const auto frame = EncodeWorkerReport(report);
  if (frame.empty()) return 1;
  // Small blocking pipe frame; only EINTR/short IPC writes are continued. No DIAG retry.
  size_t offset = 0;
  while (offset < frame.size()) {
    const auto n = write(kDiagWorkerResultFd, frame.data() + offset, frame.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 1;
    offset += static_cast<size_t>(n);
  }
  close(kDiagWorkerResultFd);
  return phase.status == "READ_OK" ? 0 : 1;
}
// v2 has only a read entry: enum -> fixed whitelist -> O_RDONLY. No write opcode.
int RunTargetReadWorker(const char* token, const char* target_text, const char* id) {
  using namespace guacamole_efs;
  WorkerNonce nonce{};
  if (!ParseWorkerNonce(token,&nonce)) return 2;
  // Canonical enum spelling only, no raw paths or permissive numeric parsing.
  std::optional<EfsTarget> target;
  for(auto t : {EfsTarget::kMcfgSelDb,EfsTarget::kMcfgRfsSwDigestVersion,
      EfsTarget::kMcfgAutoselectByUim,EfsTarget::kMcfgAutoselectByUimSubscription01,
      EfsTarget::kMcfgSetting,EfsTarget::kMcfgSetting1}) {
    if(std::to_string(static_cast<uint32_t>(t))==target_text) target=t;
  }
  if(!target) return 2;
  ProcessRequest request{ProcessOperation::kRead,*target,id,{}};
  if(!ValidProcessRequest(request)) return 2;
  struct stat st{};
  const int flags=fcntl(kDiagWorkerResultFd,F_GETFL);
  const pid_t parent=getppid();
  if(flags<0 || (flags&O_ACCMODE)!=O_WRONLY || fstat(kDiagWorkerResultFd,&st)!=0 ||
      !S_ISFIFO(st.st_mode) || parent<=1 ||
      prctl(PR_SET_PDEATHSIG,static_cast<unsigned long>(SIGKILL),0UL,0UL,0UL)!=0 || getppid()!=parent) return 2;
  ProcessReport report; report.identity=request; report.nonce=nonce;
  report.failure=ProcessFailure::kTransport;
  {
    DiagEfsSessionFactory factory([] { return std::make_unique<DiagCallbackShim>(); });
    std::string error;
    auto session=factory.OpenRead(&error);
    if(session) {
      auto read=ReadWholeFile(&session->client(),TargetPath(*target));
      if(read.state==EfsReadFileState::kDone && read.data.size()<=kProcessDataCap) {
        report.bytes=std::move(read.data); report.outcome=ProcessOutcome::kConfirmed;
        report.failure=ProcessFailure::kNone;
      }
    }
  } // Full destruction/DeInit before report. This process never initializes again.
  report.teardown_completed=true;
  report.sha256=Sha256(report.bytes.data(),report.bytes.size());
  auto frame=EncodeProcessReport(report);
  if(frame.empty()) return 1;
  size_t offset=0;
  while(offset<frame.size()) {
    auto n=write(kDiagWorkerResultFd,frame.data()+offset,frame.size()-offset);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) return 1;
    offset+=static_cast<size_t>(n);
  }
  close(kDiagWorkerResultFd);
  return report.outcome==ProcessOutcome::kConfirmed?0:1;
}
int RunProcessCycleCommand() {
  guacamole_efs::PosixReadWorkerLauncher launcher;
  const auto result = guacamole_efs::RunDiagProcessCycleReadonly(launcher,
      [](const std::string& event) {
        std::printf("event=%s\n", event.c_str());
        std::fflush(stdout);
      });
  for (size_t i = 0; i < result.workers.size(); ++i) {
    const auto& w = result.workers[i];
    const char* status = !w.spawned ? "NOT_STARTED_OR_SPAWN_FAILED" :
        w.success ? "READ_OK" : "FAILED";
    std::printf("session%zu_status=%s\n", i + 1, status);
    std::printf("session%zu_pid=%d\n", i + 1, w.pid);
    std::printf("session%zu_report_valid=%s\n", i + 1, w.report_valid ? "true" : "false");
    if (w.report_valid) std::printf("session%zu_read_status=%u\n", i + 1,
        static_cast<unsigned>(w.report.status));
    std::printf("session%zu_reaped=%s\n", i + 1, w.reaped ? "true" : "false");
    std::printf("session%zu_exit=%d\n", i + 1, w.exit_code);
    std::printf("session%zu_signal=%d\n", i + 1, w.signal);
    std::printf("session%zu_timed_out=%s\n", i + 1, w.timed_out ? "true" : "false");
    std::printf("session%zu_teardown_completed=%s\n", i + 1,
        w.report_valid && w.report.teardown_completed ? "true" : "false");
    if (w.success) {
      std::printf("session%zu_bytes=%zu\n", i + 1, w.report.bytes.size());
      std::printf("session%zu_sha256=%s\n", i + 1, Sha256Hex(w.report.sha256).c_str());
      std::printf("session%zu_data_hex=%s\n", i + 1, ToHex(w.report.bytes).c_str());
    }
    if (!w.error.empty()) std::printf("session%zu_error=%s\n", i + 1, w.error.c_str());
  }
  std::printf("exact_equal=%s\n", result.exact_equal ? "true" : "false");
  std::printf("outcome=%s\n", result.exact_equal ? "PASS" : "FAILED");
  return result.exact_equal ? 0 : 1;
}

int RunProcessSelDbReadonly() {
  using namespace guacamole_efs;
  PosixProcessWorkerLauncher launcher;
  ProcessEfsExecutor executor(launcher);
  std::vector<uint8_t> first;
  for(int i=1;i<=2;++i) {
    auto r=executor.ReadTarget(EfsTarget::kMcfgSelDb,"sel-db-readonly-validation");
    const bool ok=r.spawned && r.reaped && !r.timed_out && r.report_valid &&
        r.exit_code==0 && r.signal==0 && r.report.teardown_completed &&
        r.report.outcome==ProcessOutcome::kConfirmed;
    std::printf("session%d_status=%s\nsession%d_reaped=%s\n",i,ok?"READ_OK":"FAILED",i,r.reaped?"true":"false");
    std::printf("session%d_pid=%d\nsession%d_timed_out=%s\nsession%d_exit=%d\nsession%d_signal=%d\nsession%d_failure_category=%u\n",
        i,r.pid,i,r.timed_out?"true":"false",i,r.exit_code,i,r.signal,i,static_cast<unsigned>(r.report.failure));
    if(!ok) { std::fprintf(stderr,"session%d_error=%s\n",i,r.error.c_str()); std::printf("outcome=FAILED\n"); return 1; }
    std::printf("session%d_bytes=%zu\nsession%d_sha256=%s\n",i,r.report.bytes.size(),i,
        ToHex(std::vector<uint8_t>(r.report.sha256.begin(),r.report.sha256.end())).c_str());
    if(i==1) first=std::move(r.report.bytes);
    else if(first!=r.report.bytes) { std::printf("exact_equal=false\noutcome=FAILED\n"); return 1; }
  }
  std::printf("exact_equal=true\noutcome=PASS\n"); return 0;
}

void PrintUsage() {
  std::printf(
      "usage: guacamole_efs_probe <command>\n"
      "commands:\n"
      "  diag-process-sel-db-readonly two v2 exec-isolated sel_db reads\n"
      "  diag-process-cycle-readonly  two exec-isolated reads of fixed RFS digest\n"
      "  transport-info       Diag_LSM_Init/DeInit smoke test\n"
      "                       (real-device PASS)\n"
      "  callback-smoke        sends H.41's own known-good, non-EFS2\n"
      "                        loopback packet once and prints the raw\n"
      "                        response (real-device PASS)\n"
      "  stat-mcfg              STAT on the fixed /nv/item_files/mcfg\n"
      "                         target (implemented + host-tested; not\n"
      "                         yet run on a real device)\n"
      "  snapshot-mcfg-readonly  the integrated Cycle 1 read-only EFS2\n"
      "                          snapshot: STAT+list /nv/item_files/mcfg,\n"
      "                          read sel_db/autoselect/rfs_sw_digest,\n"
      "                          list cards/, read the 2 debug logs +\n"
      "                          the efslogging flag - one continuous\n"
      "                          run, fail-fast, no retry (implemented +\n"
      "                          host-tested; not yet run on a real\n"
      "                          device)\n"
      "  list-mcfg              not implemented this phase\n"
      "  read-sel-db            not implemented this phase\n"
      "  read-autoselect        not implemented this phase\n"
      "  read-rfs-digest        not implemented this phase\n"
      "  list-cards              not implemented this phase\n"
      "  read-debug-log          not implemented this phase\n"
      "  read-debug-old-log      not implemented this phase\n"
      "  read-efslogging-flag    not implemented this phase\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == guacamole_efs::kProcessReadCommand) {
    return argc == 5 ? RunTargetReadWorker(argv[2],argv[3],argv[4]) : 2;
  }
  if (cmd == guacamole_efs::kDiagWorkerCommand) {
    return argc == 3 ? RunDiagReadWorker(argv[2]) : 2;
  }
  if (cmd == "diag-process-sel-db-readonly") {
    return argc == 2 ? RunProcessSelDbReadonly() : 2;
  }
  if (cmd == "diag-process-cycle-readonly") {
    if (argc != 2) { PrintUsage(); return 2; }
    return RunProcessCycleCommand();
  }
  if (cmd == "diag-session-cycle-readonly") {
    std::fprintf(stderr, "BLOCKED: same-process libdiag reuse unsupported; use diag-process-cycle-readonly\n");
    return 2;
  }
  if (cmd == "transport-info") {
    return RunTransportInfo();
  }
  if (cmd == "callback-smoke") {
    return RunCallbackSmokeCommand();
  }
  if (cmd == "stat-mcfg") {
    return RunStatMcfgCommand();
  }
  if (cmd == "snapshot-mcfg-readonly") {
    return RunSnapshotMcfgReadonlyCommand();
  }
  if (cmd == "list-mcfg" || cmd == "read-sel-db" ||
      cmd == "read-autoselect" || cmd == "read-rfs-digest" ||
      cmd == "list-cards" || cmd == "read-debug-log" ||
      cmd == "read-debug-old-log" || cmd == "read-efslogging-flag") {
    return RunNotImplemented(cmd);
  }
  PrintUsage();
  return 2;
}
