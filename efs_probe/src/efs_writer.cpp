#include "efs_writer.h"

#include "diag_transport.h"
#include "efs_write_wire.h"

namespace guacamole_efs {

namespace {
EfsWriteFileResult Failed(const std::string& message, bool ambiguous) {
  EfsWriteFileResult r;
  r.state = EfsWriteFileState::kFailed;
  r.message = message;
  r.mutation_outcome_ambiguous = ambiguous;
  return r;
}

// True iff `transport_status` is kOk but the parsed response itself
// failed to parse structurally - a confirmed round trip that tells us
// nothing about what the modem actually did (item B/2, 2026-09-24 W1
// Finalization Round). `parsed_ok` is the specific EfsOpenResult/
// EfsWriteResult/EfsCloseResult's own `ok` field.
bool IsMalformedMutatingResponse(TransportStatus transport_status,
                                  bool parsed_ok) {
  return transport_status == TransportStatus::kOk && !parsed_ok;
}
}  // namespace

EfsWriteFileResult WriteWholeFile(IEfsWriteClient* client, EfsWriteTarget target,
                                   const std::vector<uint8_t>& data) {
  if (data.size() > kMaxWriteFileSizeBytes) {
    return Failed("data size " + std::to_string(data.size()) +
                       " exceeds this project's own kMaxWriteFileSizeBytes cap (" +
                       std::to_string(kMaxWriteFileSizeBytes) + ")",
                   /*ambiguous=*/false);
  }

  bool ambiguous = false;

  MutationOpenResult open_r = client->OpenWrite(target);
  bool open_ambiguous = IsMutationOutcomeAmbiguous(open_r.transport_status) ||
                         IsMalformedMutatingResponse(open_r.transport_status,
                                                      open_r.parsed.ok);
  ambiguous = ambiguous || open_ambiguous;
  if (open_r.transport_status != TransportStatus::kOk || !open_r.parsed.ok ||
      open_r.parsed.diag_errno != 0 || open_r.parsed.fd < 0) {
    // No fd was ever obtained - nothing to close, ambiguous or not.
    return Failed("open: failed", ambiguous);
  }
  int32_t fd = open_r.parsed.fd;

  uint32_t offset = 0;
  uint32_t remaining = static_cast<uint32_t>(data.size());
  while (remaining > 0) {
    uint32_t chunk =
        remaining < kMaxWriteRequestBytes ? remaining : kMaxWriteRequestBytes;
    std::vector<uint8_t> chunk_data(data.begin() + offset,
                                     data.begin() + offset + chunk);
    MutationWriteResult w = client->Write(fd, offset, chunk_data);

    bool write_ambiguous = IsMutationOutcomeAmbiguous(w.transport_status) ||
                            IsMalformedMutatingResponse(w.transport_status,
                                                         w.parsed.ok);
    if (write_ambiguous) {
      // AMBIGUOUS WRITE (item C, 2026-09-24 W1 Finalization Round): the
      // modem's actual state after this WRITE is NOT confirmed - it may
      // have applied a partial or full write despite this call
      // reporting failure. Do NOT call Close() on this (possibly
      // poisoned) session: a second unconfirmed mutating request on top
      // of the first cannot be trusted to be safe either, and would
      // only compound the uncertainty. Stop immediately and dispose of
      // this session - the caller (efs_transaction.h's ApplyPatchPlan)
      // is responsible for resolving this via an INDEPENDENT read-back,
      // never by reusing this client.
      return Failed("write: ambiguous mutation outcome at offset " +
                         std::to_string(offset) +
                         " - session abandoned, no same-session close "
                         "attempted",
                     /*ambiguous=*/true);
    }

    // Below this point, transport_status == kOk AND parsed.ok == true -
    // a fully confirmed round trip. Any remaining failure is a
    // CONFIRMED, non-ambiguous outcome: the session is known healthy,
    // so a best-effort Close() is safe and appropriate.
    if (w.parsed.diag_errno != 0) {
      auto close_r = client->Close(fd);
      ambiguous = ambiguous || IsMutationOutcomeAmbiguous(close_r.transport_status) ||
                  IsMalformedMutatingResponse(close_r.transport_status,
                                               close_r.parsed.ok);
      return Failed("write: diag_errno=" + std::to_string(w.parsed.diag_errno) +
                         " at offset " + std::to_string(offset),
                     ambiguous);
    }
    // SHORT WRITE POLICY (see this file's header comment): anything
    // other than EXACTLY the requested chunk size is a hard failure,
    // never a silently-accepted partial write. This is NOT an
    // ambiguous outcome by itself - the transport round trip was
    // clean (transport_status == kOk, parsed.ok == true), we KNOW
    // exactly what the modem reported.
    if (w.parsed.bytes_written != static_cast<int32_t>(chunk)) {
      auto close_r = client->Close(fd);
      ambiguous = ambiguous || IsMutationOutcomeAmbiguous(close_r.transport_status) ||
                  IsMalformedMutatingResponse(close_r.transport_status,
                                               close_r.parsed.ok);
      return Failed("write: short write - wrote " +
                         std::to_string(w.parsed.bytes_written) +
                         " of requested " + std::to_string(chunk) +
                         " bytes at offset " + std::to_string(offset),
                     ambiguous);
    }
    offset += chunk;
    remaining -= chunk;
  }

  MutationCloseResult close_r = client->Close(fd);
  ambiguous = ambiguous || IsMutationOutcomeAmbiguous(close_r.transport_status) ||
              IsMalformedMutatingResponse(close_r.transport_status,
                                           close_r.parsed.ok);
  if (close_r.transport_status != TransportStatus::kOk || !close_r.parsed.ok ||
      close_r.parsed.errno_value != 0) {
    return Failed("close: failed after a fully-successful write", ambiguous);
  }

  EfsWriteFileResult result;
  result.mutation_outcome_ambiguous = ambiguous;
  result.total_bytes_written = offset;
  if (result.total_bytes_written != data.size()) {
    // Should be unreachable given the loop's own invariant (checked
    // explicitly anyway, exactly like efs_reader.cpp's analogous final
    // check - never trust an invariant silently).
    result.state = EfsWriteFileState::kFailed;
    result.message = "total bytes written (" +
                      std::to_string(result.total_bytes_written) +
                      ") != requested data size (" +
                      std::to_string(data.size()) + ")";
    return result;
  }

  result.state = EfsWriteFileState::kDone;
  return result;
}

}  // namespace guacamole_efs
