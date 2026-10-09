#include "skt_load.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "hash.h"
#include "known_profiles.h"

namespace guacamole_pdc {

const char* LoadStateToString(LoadState state) {
  switch (state) {
    case LoadState::kPrecheck: return "PRECHECK";
    case LoadState::kDiscover: return "DISCOVER";
    case LoadState::kOpenFile: return "OPEN_FILE";
    case LoadState::kLoadBegin: return "LOAD_BEGIN";
    case LoadState::kLoadChunks: return "LOAD_CHUNKS";
    case LoadState::kWaitCompletion: return "WAIT_COMPLETION";
    case LoadState::kVerifyList: return "VERIFY_LIST";
    case LoadState::kVerifyInfo: return "VERIFY_INFO";
    case LoadState::kDone: return "DONE";
    case LoadState::kUnknownResult: return "UNKNOWN_RESULT";
    case LoadState::kFailed: return "FAILED";
  }
  return "UNKNOWN";
}

const char* SelectionCheckResultToString(SelectionCheckResult result) {
  switch (result) {
    case SelectionCheckResult::kUnchanged: return "UNCHANGED";
    case SelectionCheckResult::kChanged: return "CHANGED";
    case SelectionCheckResult::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

namespace {

// DIAGNOSTIC ONLY - see ConfigPresence's comment in pdc_client.h. Never
// used to decide whether a config exists; only to populate
// `found_in_list_after` for the operator's own reference.
bool ListContainsId(const ListConfigsResult& list,
                     const std::vector<uint8_t>& id) {
  for (const auto& cfg : list.configs) {
    if (cfg.type == ConfigType::kSoftware && cfg.id == id) return true;
  }
  return false;
}

SelectedSnapshot QuerySelected(IPdcClient& client, uint32_t timeout_ms) {
  SelectedSnapshot snap;
  GetSelectedConfigOutcome outcome =
      client.GetSelectedConfig(ConfigType::kSoftware, timeout_ms);
  if (outcome.status != ProbeStatus::kOk) {
    return snap;  // queried stays false - caller must not treat this as NONE.
  }
  snap.queried = true;
  snap.active_id = outcome.data.active_id;
  snap.pending_id = outcome.data.pending_id;
  return snap;
}

SelectionCheckResult CompareSelection(const SelectedSnapshot& before,
                                       const SelectedSnapshot& after) {
  if (!before.queried || !after.queried) return SelectionCheckResult::kUnknown;
  if (before.active_id == after.active_id &&
      before.pending_id == after.pending_id) {
    return SelectionCheckResult::kUnchanged;
  }
  return SelectionCheckResult::kChanged;
}

// AUTHORITATIVE existence check: Get Config Info on the exact config ID,
// classified via ClassifyConfigPresence (pdc_wire.cpp). Never List
// Configs - see ConfigPresence's comment in pdc_client.h.
ConfigPresence QueryPresence(IPdcClient& client,
                              const std::vector<uint8_t>& config_id,
                              uint32_t timeout_ms,
                              GetConfigInfoOutcome* out_info) {
  *out_info =
      client.GetConfigInfo(ConfigType::kSoftware, config_id, timeout_ms);
  return ClassifyConfigPresence(*out_info);
}

// Best-effort, read-only re-check run after a mid-transfer failure (or
// a timeout), so the operator gets the same diagnostic picture a manual
// `list`/`info`/`selected` follow-up would have given them -
// automatically, and WITHOUT ever overwriting the failure that
// triggered it. `config_presence_after`/`info_after` (from the
// AUTHORITATIVE Get Config Info call) and `found_in_list_after` (List
// Configs, DIAGNOSTIC ONLY) / `selected_after` / `selection_check` are
// all populated here; `final_state`/`message` are never modified here.
void RunPostMortemVerification(IPdcClient& client,
                                const std::vector<uint8_t>& config_id,
                                uint32_t timeout_ms, SktLoadReport* report) {
  ListConfigsOutcome list = client.ListConfigs(timeout_ms);
  if (list.status == ProbeStatus::kOk) {
    report->found_in_list_after = ListContainsId(list.data, config_id);
  }
  report->config_presence_after =
      QueryPresence(client, config_id, timeout_ms, &report->info_after);
  report->selected_after = QuerySelected(client, timeout_ms);
  report->selection_check =
      CompareSelection(report->selected_before, report->selected_after);
}

// Reads the whole file into `out`, verifying it's a regular file of
// exactly `profile.expected_size` bytes. Returns a human-readable error
// string on any failure, or std::nullopt on success. Never partially
// trusts a short read - either the whole expected size comes back, or
// this is treated as a failure, not a truncated-but-usable buffer.
std::optional<std::string> ReadAndSizeCheckFile(const KnownProfile& profile,
                                                  std::vector<uint8_t>* out) {
  int fd = open(profile.runtime_path, O_RDONLY);
  if (fd < 0) {
    return std::string("open() failed for ") + profile.runtime_path;
  }
  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    return std::string("fstat() failed for ") + profile.runtime_path;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return std::string(profile.runtime_path) + " is not a regular file";
  }
  if (st.st_size != static_cast<off_t>(profile.expected_size)) {
    close(fd);
    return "size mismatch: file is " + std::to_string(st.st_size) +
           " bytes, expected " + std::to_string(profile.expected_size);
  }

  out->assign(static_cast<size_t>(st.st_size), 0);
  size_t total_read = 0;
  while (total_read < out->size()) {
    ssize_t n = read(fd, out->data() + total_read, out->size() - total_read);
    if (n <= 0) {
      close(fd);
      return "short read: got " + std::to_string(total_read) + " of " +
             std::to_string(out->size()) + " bytes";
    }
    total_read += static_cast<size_t>(n);
  }
  close(fd);
  return std::nullopt;
}

}  // namespace

SktLoadReport RunLoadSkt(IPdcClient& client, uint32_t timeout_ms) {
  const KnownProfile& profile = GetSktProfile();

  std::vector<uint8_t> file_bytes;
  auto err = ReadAndSizeCheckFile(profile, &file_bytes);
  if (err) {
    SktLoadReport report;
    report.final_state = LoadState::kFailed;
    report.message = "PRECHECK failed: " + *err;
    return report;
  }

  return RunLoadFromBytes(client, file_bytes, profile, timeout_ms);
}

SktLoadReport RunLoadCarrier(IPdcClient& client, Carrier carrier,
                              uint32_t timeout_ms) {
  const KnownProfile* selected = GetProfileForCarrier(carrier);
  if (!selected) { SktLoadReport r; r.final_state=LoadState::kFailed; r.message="invalid carrier"; return r; }
  const KnownProfile& profile = *selected;

  std::vector<uint8_t> file_bytes;
  auto err = ReadAndSizeCheckFile(profile, &file_bytes);
  if (err) {
    SktLoadReport report;
    report.final_state = LoadState::kFailed;
    report.message = std::string(CarrierToString(carrier)) +
                      " PRECHECK failed: " + *err;
    return report;
  }

  return RunLoadFromBytes(client, file_bytes, profile, timeout_ms);
}

SktLoadReport RunLoadFromBytes(IPdcClient& client,
                                const std::vector<uint8_t>& file_bytes,
                                const KnownProfile& profile,
                                uint32_t timeout_ms) {
  SktLoadReport report;

  // ---- PRECHECK (size check already done by the caller for the real
  // filesystem path; re-check here too since RunLoadFromBytes can also
  // be called directly by tests with synthetic bytes/profile pairs) ----
  report.final_state = LoadState::kPrecheck;
  if (file_bytes.size() != profile.expected_size) {
    report.final_state = LoadState::kFailed;
    report.message = "PRECHECK failed: size mismatch: got " +
                      std::to_string(file_bytes.size()) + " bytes, expected " +
                      std::to_string(profile.expected_size);
    return report;
  }
  report.file_size = static_cast<uint32_t>(file_bytes.size());

  auto sha256 = Sha256(file_bytes.data(), file_bytes.size());
  bool sha256_match = true;
  for (size_t i = 0; i < 32; i++) {
    if (sha256[i] != profile.expected_sha256[i]) {
      sha256_match = false;
      break;
    }
  }
  if (!sha256_match) {
    report.final_state = LoadState::kFailed;
    report.message =
        "PRECHECK failed: SHA-256 of on-device file does not match the "
        "known-good stock SKT MBN - refusing to load an unverified file "
        "(fail-closed)";
    return report;
  }

  auto sha1 = Sha1(file_bytes.data(), file_bytes.size());
  report.config_id.assign(sha1.begin(), sha1.end());
  report.final_state = LoadState::kOpenFile;  // precheck + open both clean.

  // ---- DISCOVER: AUTHORITATIVE pre-load existence check via Get Config
  // Info on the exact config ID - NEVER List Configs (see ConfigPresence's
  // comment in pdc_client.h: a real-device Phase C0 test found List
  // Configs can silently omit an entry Get Config Info still resolves
  // successfully). Also captures the pre-load `selected` baseline. Both
  // must succeed before any write is attempted - if we can't observe the
  // "before" state, we have no way to detect the safety violation this
  // whole phase exists to catch (Load implicitly changing selection), so
  // we refuse to proceed rather than write blind. ----
  report.final_state = LoadState::kDiscover;
  report.config_presence_before =
      QueryPresence(client, report.config_id, timeout_ms, &report.info_before);

  if (report.config_presence_before == ConfigPresence::kExists) {
    report.already_existed_before_load = true;
    report.config_presence_after = ConfigPresence::kExists;  // unchanged - no write attempted.
    report.info_after = report.info_before;
    report.final_state = LoadState::kDone;
    report.message =
        "SKT config already present in PDC config DB (confirmed via Get "
        "Config Info on the exact SHA-1 id) - skipping Load entirely, no "
        "write performed.";
    return report;
  }
  if (report.config_presence_before == ConfigPresence::kUnknown) {
    report.final_state = LoadState::kFailed;
    report.message =
        std::string(
            "DISCOVER failed: pre-load Get Config Info presence check was "
            "inconclusive (status=") +
        ProbeStatusToString(report.info_before.status) +
        ", error_status=" + std::to_string(report.info_before.data.error_status) +
        ") - neither a confirmed EXISTS nor a confirmed ABSENT "
        "(QMI_PROTOCOL_ERROR_NO_ENTRY). Failing closed rather than risk a "
        "duplicate or unverifiable Load.";
    return report;
  }
  // kAbsent - confirmed not present, safe to proceed.

  report.selected_before = QuerySelected(client, timeout_ms);
  if (!report.selected_before.queried) {
    report.final_state = LoadState::kFailed;
    report.message =
        "DISCOVER failed: Get Selected Config (before) did not succeed - "
        "cannot establish a safety baseline, refusing to load.";
    return report;
  }

  // ---- LOAD_BEGIN / LOAD_CHUNKS / WAIT_COMPLETION ----
  //
  // Indication field semantics, CONFIRMED against real H.41 hardware on
  // the first Phase C0 device test (see PHASE_C0_FINDINGS.md's
  // "real-device test #1" section) and independently cross-checked
  // against libqmi's own public reference client
  // (qmicli-pdc.c:load_config_ready_indication, fetched and read
  // directly): the REFERENCE IMPLEMENTATION NEVER READS "Received" AT
  // ALL - progress control is driven exclusively by "Remaining Size"
  // (loop until remaining_size == 0) and "Frame Reset" (abort
  // immediately, never auto-resend). A second real-device test
  // ("real-device test #2") confirmed the fixed logic below transfers
  // the real 41172-byte stock SKT MBN across all 41 chunks successfully.
  report.final_state = LoadState::kLoadBegin;
  bool timed_out = false;
  std::string chunk_failure_message;

  for (size_t offset = 0; offset < file_bytes.size();
       offset += kLoadChunkSize) {
    report.final_state = LoadState::kLoadChunks;
    const size_t this_chunk_len =
        std::min(kLoadChunkSize, file_bytes.size() - offset);

    LoadConfigChunkOutcome chunk = client.LoadConfigChunk(
        ConfigType::kSoftware, report.config_id, report.file_size,
        file_bytes.data() + offset, this_chunk_len, timeout_ms);

    if (chunk.status == ProbeStatus::kTimeout) {
      // Explicitly NOT retried - policy requires re-verification (Get
      // Config Info, authoritative; List Configs, diagnostic) instead
      // of resending.
      timed_out = true;
      break;
    }
    if (chunk.status != ProbeStatus::kOk) {
      chunk_failure_message =
          std::string("chunk at offset ") + std::to_string(offset) +
          " failed: " + ProbeStatusToString(chunk.status) +
          " (error_status=" + std::to_string(chunk.data.error_status) + ")";
      break;
    }

    report.final_state = LoadState::kWaitCompletion;
    report.chunks_sent++;
    report.bytes_sent += static_cast<uint32_t>(this_chunk_len);

    // "Received" is OPTIONAL/ADVISORY (not marked mandatory in the
    // public PDC schema, and the reference client never reads it at
    // all). When present, it describes THIS chunk's own size, not a
    // running total - verify only that much, and never fail the whole
    // transfer just because the TLV happens to be absent.
    if (chunk.data.has_received &&
        chunk.data.received != static_cast<uint32_t>(this_chunk_len)) {
      chunk_failure_message =
          "chunk at offset " + std::to_string(offset) +
          ": modem-reported Received (" + std::to_string(chunk.data.received) +
          ") does not match this chunk's own size (" +
          std::to_string(this_chunk_len) +
          ") - Received is per-current-chunk, not cumulative (confirmed "
          "against real hardware and libqmi's reference client)";
      break;
    }

    // "Remaining Size" IS the authoritative progress field (required -
    // matches the reference client treating its absence as an error).
    const uint32_t cumulative_sent =
        static_cast<uint32_t>(offset) + static_cast<uint32_t>(this_chunk_len);
    const uint32_t expected_remaining = report.file_size - cumulative_sent;
    if (!chunk.data.has_remaining_size ||
        chunk.data.remaining_size != expected_remaining) {
      chunk_failure_message =
          "chunk at offset " + std::to_string(offset) +
          ": modem-reported Remaining Size (" +
          std::to_string(chunk.data.has_remaining_size
                              ? chunk.data.remaining_size
                              : 0) +
          ") does not match expected (" + std::to_string(expected_remaining) +
          ")";
      break;
    }
    if (chunk.data.has_frame_reset && chunk.data.frame_reset) {
      chunk_failure_message =
          "chunk at offset " + std::to_string(offset) +
          ": modem signaled Frame Reset (treated as rejection/abort per "
          "protocol semantics - never auto-resent)";
      break;
    }

    if (chunk.data.remaining_size == 0) {
      break;  // last chunk acknowledged, transfer complete.
    }
  }

  if (!chunk_failure_message.empty()) {
    report.final_state = LoadState::kFailed;
    report.message = "LOAD_CHUNKS failed: " + chunk_failure_message;
    // Best-effort, read-only post-mortem - never overwrites the failure
    // above, just enriches the report with what a manual follow-up
    // list/info/selected would have shown.
    RunPostMortemVerification(client, report.config_id, timeout_ms, &report);
    return report;
  }

  if (timed_out) {
    // Per policy: do not resend. Re-query (Get Config Info, the
    // authoritative answer; List Configs, diagnostic only) to find out
    // what actually happened server-side.
    report.final_state = LoadState::kUnknownResult;
    RunPostMortemVerification(client, report.config_id, timeout_ms, &report);
    if (report.config_presence_after == ConfigPresence::kExists) {
      report.message =
          "A chunk timed out waiting for its indication, so completion "
          "could not be confirmed directly. A post-timeout Get Config Info "
          "re-check on the exact config ID (authoritative) DOES confirm the "
          "SKT config now exists - persistence is inferred, not directly "
          "confirmed by a clean completion indication. Treat with extra "
          "caution.";
    } else if (report.config_presence_after == ConfigPresence::kAbsent) {
      report.message =
          "A chunk timed out waiting for its indication, and a post-timeout "
          "Get Config Info re-check on the exact config ID (authoritative) "
          "confirms the SKT config does NOT exist. Outcome genuinely "
          "unknown at the point of timeout, but the eventual state is "
          "clear: not loaded. Do not retry automatically.";
    } else {
      report.message =
          "A chunk timed out waiting for its indication, and the "
          "post-timeout Get Config Info re-check was ITSELF inconclusive "
          "(presence=UNKNOWN). Outcome genuinely unknown; do not assume "
          "success or failure, do not retry automatically.";
    }
    return report;
  }

  // ---- VERIFY_LIST (DIAGNOSTIC ONLY - never gates the result; see
  // ConfigPresence's comment in pdc_client.h) ----
  report.final_state = LoadState::kVerifyList;
  ListConfigsOutcome list_after = client.ListConfigs(timeout_ms);
  if (list_after.status == ProbeStatus::kOk) {
    report.found_in_list_after =
        ListContainsId(list_after.data, report.config_id);
  }

  // ---- VERIFY_INFO (AUTHORITATIVE: Get Config Info on the exact config
  // ID is the sole basis for declaring success here) ----
  report.final_state = LoadState::kVerifyInfo;
  report.config_presence_after =
      QueryPresence(client, report.config_id, timeout_ms, &report.info_after);

  if (report.config_presence_after != ConfigPresence::kExists) {
    report.final_state = LoadState::kFailed;
    report.message =
        std::string(
            "VERIFY_INFO failed: chunk transfer reported clean completion "
            "(Remaining Size reached 0 with no error), but a post-load Get "
            "Config Info on the exact config ID (authoritative) reports "
            "presence=") +
        ConfigPresenceToString(report.config_presence_after) +
        ", not EXISTS. (List Configs, diagnostic only, " +
        (report.found_in_list_after ? "DID" : "did NOT") +
        " show the id - not used to make this determination.) Reporting as "
        "failed rather than assuming success.";
    report.selected_after = QuerySelected(client, timeout_ms);
    report.selection_check =
        CompareSelection(report.selected_before, report.selected_after);
    return report;
  }
  if (!report.info_after.data.has_total_size ||
      report.info_after.data.total_size != report.file_size) {
    report.final_state = LoadState::kFailed;
    report.message =
        "VERIFY_INFO failed: Get Config Info confirms the config exists, "
        "but its reported total_size (" +
        std::to_string(report.info_after.data.has_total_size
                            ? report.info_after.data.total_size
                            : 0) +
        ") does not match the loaded file size (" +
        std::to_string(report.file_size) + ")";
    report.selected_after = QuerySelected(client, timeout_ms);
    report.selection_check =
        CompareSelection(report.selected_before, report.selected_after);
    return report;
  }

  // ---- Before/after `selected` safety check - the whole point of C0 ----
  report.selected_after = QuerySelected(client, timeout_ms);
  report.selection_check =
      CompareSelection(report.selected_before, report.selected_after);

  if (report.selection_check != SelectionCheckResult::kUnchanged) {
    report.final_state = LoadState::kFailed;
    report.message =
        std::string(
            "Load Config transferred and verified successfully (Get Config "
            "Info confirms the config exists with the correct size), BUT "
            "the post-load selected-config safety check is ") +
        SelectionCheckResultToString(report.selection_check) +
        " (not confirmed UNCHANGED) - this violates the core C0 safety "
        "requirement that Load must not implicitly select/activate. "
        "Flagging as FAILED/risk despite the successful transfer.";
    return report;
  }

  report.final_state = LoadState::kDone;
  report.message =
      "SKT config loaded and verified via Get Config Info on the exact "
      "config ID (size-confirmed), and active/pending selection is "
      "confirmed unchanged. Reboot-persistence has NOT been tested yet in "
      "this run - see the printed manual verification steps.";
  return report;
}

}  // namespace guacamole_pdc
