#include "efs_writer.h"

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {

// Fake IEfsWriteClient: fully scripted, records call counts and every
// WRITE call's exact (offset, data) - so tests can assert "no retry"
// and "exact chunk ordering" directly, mirroring efs_reader_test.cpp's
// FakeEfsClient pattern.
class FakeEfsWriteClient : public IEfsWriteClient {
 public:
  MutationOpenResult open_response;
  std::vector<MutationWriteResult> write_responses;
  MutationCloseResult close_response;

  int open_calls = 0;
  int write_calls = 0;
  int close_calls = 0;
  std::vector<uint32_t> requested_offsets;
  std::vector<std::vector<uint8_t>> requested_chunks;

  MutationOpenResult OpenWrite(EfsWriteTarget) override {
    open_calls++;
    return open_response;
  }
  MutationWriteResult Write(int32_t, uint32_t offset,
                             const std::vector<uint8_t>& data) override {
    requested_offsets.push_back(offset);
    requested_chunks.push_back(data);
    if (static_cast<size_t>(write_calls) >= write_responses.size()) {
      MutationWriteResult r;  // ran off the script - hard failure, no
                                // implicit retry (transport_status stays
                                // its default kInitFailed, parsed.ok
                                // stays false).
      write_calls++;
      return r;
    }
    return write_responses[write_calls++];
  }
  MutationCloseResult Close(int32_t) override {
    close_calls++;
    return close_response;
  }
};

MutationOpenResult MakeOpen(int32_t fd = 3, int32_t diag_errno = 0) {
  MutationOpenResult o;
  o.transport_status = TransportStatus::kOk;
  o.parsed.ok = true;
  o.parsed.fd = fd;
  o.parsed.diag_errno = diag_errno;
  return o;
}

MutationCloseResult MakeClose(int32_t errno_value = 0) {
  MutationCloseResult c;
  c.transport_status = TransportStatus::kOk;
  c.parsed.ok = true;
  c.parsed.errno_value = errno_value;
  return c;
}

MutationWriteResult MakeWrite(int32_t fd, uint32_t offset,
                               int32_t bytes_written, int32_t diag_errno = 0) {
  MutationWriteResult r;
  r.transport_status = TransportStatus::kOk;
  r.parsed.ok = true;
  r.parsed.fd = fd;
  r.parsed.offset = offset;
  r.parsed.bytes_written = bytes_written;
  r.parsed.diag_errno = diag_errno;
  return r;
}

constexpr EfsWriteTarget kTarget = EfsWriteTarget::kMcfgSelDb;

}  // namespace

TEST(EfsWriter, EmptyDataTruncatesOnly) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen();
  c.close_response = MakeClose();

  auto r = WriteWholeFile(&c, kTarget, {});
  ASSERT_EQ(r.state, EfsWriteFileState::kDone);
  EXPECT_EQ(r.total_bytes_written, 0u);
  EXPECT_FALSE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.open_calls, 1);   // OPEN(write-truncate) still happens.
  EXPECT_EQ(c.write_calls, 0);  // nothing to write.
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsWriter, Exact1024Boundary) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.write_responses = {MakeWrite(5, 0, 1024)};

  std::vector<uint8_t> data(1024, 0xAB);
  auto r = WriteWholeFile(&c, kTarget, data);
  ASSERT_EQ(r.state, EfsWriteFileState::kDone);
  EXPECT_EQ(r.total_bytes_written, 1024u);
  EXPECT_EQ(c.write_calls, 1);
}

TEST(EfsWriter, MultiChunkWriteSucceeds) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(7);
  c.close_response = MakeClose();
  c.write_responses = {
      MakeWrite(7, 0, 1024),
      MakeWrite(7, 1024, 1024),
      MakeWrite(7, 2048, 452),
  };

  std::vector<uint8_t> data(2500, 0x03);
  auto r = WriteWholeFile(&c, kTarget, data);
  ASSERT_EQ(r.state, EfsWriteFileState::kDone);
  EXPECT_EQ(r.total_bytes_written, 2500u);
  EXPECT_EQ(c.write_calls, 3);
}

TEST(EfsWriter, OffsetAdvancesExactly) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(7);
  c.close_response = MakeClose();
  c.write_responses = {
      MakeWrite(7, 0, 1024),
      MakeWrite(7, 1024, 1024),
      MakeWrite(7, 2048, 452),
  };

  std::vector<uint8_t> data(2500, 0x03);
  auto r = WriteWholeFile(&c, kTarget, data);
  ASSERT_EQ(r.state, EfsWriteFileState::kDone);
  ASSERT_EQ(c.requested_offsets.size(), 3u);
  EXPECT_EQ(c.requested_offsets[0], 0u);
  EXPECT_EQ(c.requested_offsets[1], 1024u);
  EXPECT_EQ(c.requested_offsets[2], 2048u);
  EXPECT_EQ(c.requested_chunks[0].size(), 1024u);
  EXPECT_EQ(c.requested_chunks[1].size(), 1024u);
  EXPECT_EQ(c.requested_chunks[2].size(), 452u);
}

TEST(EfsWriter, ChunkContentMatchesSourceData) {
  // The chunk sent to the client must be the EXACT bytes of the source
  // `data` - not just the right length. (Chunking itself, across
  // multiple WRITE calls, is already covered by MultiChunkWriteSucceeds/
  // OffsetAdvancesExactly - this test isolates content-exactness for
  // the common single-chunk case.)
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.write_responses = {MakeWrite(5, 0, 5)};

  std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55};
  auto r = WriteWholeFile(&c, kTarget, data);
  ASSERT_EQ(r.state, EfsWriteFileState::kDone);
  ASSERT_EQ(c.requested_chunks.size(), 1u);
  EXPECT_EQ(c.requested_chunks[0], data);
}

TEST(EfsWriter, ShortWriteRejected) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  // Requests a 1024-byte chunk but the (fake) modem reports writing
  // only 500 - structurally valid per the wire parser, but this
  // orchestration layer's own short-write policy must reject it. This
  // is a CLEAN transport round trip (transport_status=kOk) so it must
  // NOT be flagged ambiguous.
  c.write_responses = {MakeWrite(5, 0, 500)};

  std::vector<uint8_t> data(1024, 0xAB);
  auto r = WriteWholeFile(&c, kTarget, data);
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_FALSE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.close_calls, 1);  // best-effort close still attempted.
  EXPECT_EQ(c.write_calls, 1);  // no retry of the short chunk.
}

TEST(EfsWriter, ZeroBytesWrittenRejected) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  c.write_responses = {MakeWrite(5, 0, 0)};

  std::vector<uint8_t> data(10, 0xAB);
  auto r = WriteWholeFile(&c, kTarget, data);
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsWriter, DiagErrnoWriteIsNotRetried) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  auto err_write = MakeWrite(5, 0, 0);
  err_write.parsed.diag_errno = 5;
  c.write_responses = {err_write};

  std::vector<uint8_t> data(10, 0xAB);
  auto r = WriteWholeFile(&c, kTarget, data);
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_FALSE(r.mutation_outcome_ambiguous);  // clean round trip, modem said no.
  EXPECT_EQ(c.write_calls, 1);  // exactly one attempt - no retry.
}

TEST(EfsWriter, OpenFailureIsFailClosed) {
  FakeEfsWriteClient c;
  MutationOpenResult bad;  // default: transport_status=kInitFailed.
  c.open_response = bad;

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.write_calls, 0);
  EXPECT_EQ(c.close_calls, 0);  // never opened - nothing to close.
}

TEST(EfsWriter, OpenDiagErrnoIsFailClosed) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5, /*diag_errno=*/13);  // e.g. EACCES-style.

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.write_calls, 0);
}

TEST(EfsWriter, WriteFailureStillClosesHandle) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  // write_responses left empty - the fake's "ran off script" path
  // returns a default (transport_status=kInitFailed) result.

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.close_calls, 1);
}

TEST(EfsWriter, MalformedWriteResponseIsAmbiguousAndAbandonsSession) {
  // 2026-09-24 W1 Finalization Round, item B: a clean transport round
  // trip (transport_status == kOk) whose response the parser could not
  // structurally parse (parsed.ok == false) no longer means "we know
  // for certain the write failed" - the WRITE's mutating side effect on
  // the modem is NOT confirmed either way, so this is now AMBIGUOUS
  // (REVERSED from the prior EXPECT_FALSE), and per item C, no
  // same-session Close() is attempted.
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult malformed;
  malformed.transport_status = TransportStatus::kOk;  // clean round trip,
  malformed.parsed.ok = false;                          // parser rejected it.
  c.write_responses = {malformed};

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.close_calls, 0);  // session abandoned, not closed.
}

TEST(EfsWriter, CloseFailureAfterSuccessfulWriteIsFailed) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.write_responses = {MakeWrite(5, 0, 3)};
  MutationCloseResult bad_close;
  bad_close.transport_status = TransportStatus::kOk;
  bad_close.parsed.ok = true;
  bad_close.parsed.errno_value = 5;  // structurally parsed, but CLOSE itself failed.
  c.close_response = bad_close;

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02, 0x03});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
}

TEST(EfsWriter, OversizedDataRejectedBeforeOpen) {
  FakeEfsWriteClient c;
  std::vector<uint8_t> data(kMaxWriteFileSizeBytes + 1, 0xAB);

  auto r = WriteWholeFile(&c, kTarget, data);
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.open_calls, 0);  // rejected before ever opening.
}

TEST(EfsWriter, TimeoutIsNotRetried) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult timeout_write;
  timeout_write.transport_status = TransportStatus::kTimeout;
  c.write_responses = {timeout_write};

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_EQ(c.write_calls, 1);  // exactly one attempt - no retry.
  // 2026-09-24 W1 Finalization Round, item C: a TIMEOUT is an ambiguous
  // WRITE outcome - no same-session Close() is attempted (REVERSED from
  // the prior close_calls==1 expectation).
  EXPECT_EQ(c.close_calls, 0);
}

// --- Transport ambiguity propagation (2026-09-24 Gate W1 preflight
// hardening) - see this file's WriteWholeFile() header comment. ---

TEST(EfsWriter, WriteTimeoutMarksResultAmbiguous) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult timeout_write;
  timeout_write.transport_status = TransportStatus::kTimeout;
  c.write_responses = {timeout_write};

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.close_calls, 0);  // item C: no same-session close.
}

TEST(EfsWriter, OpenTimeoutMarksResultAmbiguous) {
  FakeEfsWriteClient c;
  MutationOpenResult timeout_open;
  timeout_open.transport_status = TransportStatus::kTimeout;
  c.open_response = timeout_open;

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
}

TEST(EfsWriter, CloseTimeoutMarksResultAmbiguousEvenAfterSuccessfulWrite) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.write_responses = {MakeWrite(5, 0, 2)};
  MutationCloseResult timeout_close;
  timeout_close.transport_status = TransportStatus::kTimeout;
  c.close_response = timeout_close;

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02});
  // The write itself is confirmed exact, but the ambiguous CLOSE still
  // taints the overall result - conservative by design (see this
  // file's header comment).
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);  // CLOSE itself failed.
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
}

TEST(EfsWriter, FrameIntegrityFailureMarksResultAmbiguous) {
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult frame_fail;
  frame_fail.transport_status = TransportStatus::kFrameIntegrityFailed;
  c.write_responses = {frame_fail};

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
}

TEST(EfsWriter, SendFailedIsAmbiguousForMutatingWrite) {
  // 2026-09-24 W1 Finalization Round, item B/3 (REVERSED from
  // SendFailedIsNotAmbiguous): the assumption that a synchronous
  // SendData() failure means "the request never reached the modem" is
  // NOT proven by any libdiag reverse-engineering evidence for this
  // firmware - see diag_transport.h's IsMutationOutcomeAmbiguous(). For
  // a MUTATING request, kSendFailed is now treated conservatively as
  // ambiguous, and per item C no same-session Close() is attempted.
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult send_failed;
  send_failed.transport_status = TransportStatus::kSendFailed;
  c.write_responses = {send_failed};

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.close_calls, 0);
}

TEST(EfsWriter, SessionDesynchronizedIsNotAmbiguous) {
  // Rejected BEFORE ever touching the shim (a prior request already
  // poisoned the session) - this specific call definitely never sent
  // anything either. Unlike kSendFailed, this is a LOCAL guard
  // (DiagTransport refuses the call before touching the shim at all -
  // see diag_transport.h), not dependent on any unproven libdiag
  // send-path assumption, so it stays non-ambiguous.
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult desync;
  desync.transport_status = TransportStatus::kSessionDesynchronized;
  c.write_responses = {desync};

  auto r = WriteWholeFile(&c, kTarget, {0x01});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_FALSE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.close_calls, 1);  // non-ambiguous - session known healthy.
}

// --- New tests, 2026-09-24 W1 Finalization Round, item C. ---

TEST(EfsWriter, AmbiguousWriteFailureDoesNotClosePoisonedSession) {
  // A multi-chunk write where the FIRST chunk's WRITE already comes
  // back ambiguous: the loop must stop immediately (never attempt the
  // second chunk) and must never call Close() on this session.
  FakeEfsWriteClient c;
  c.open_response = MakeOpen(5);
  c.close_response = MakeClose();
  MutationWriteResult frame_fail;
  frame_fail.transport_status = TransportStatus::kFrameIntegrityFailed;
  c.write_responses = {frame_fail};  // only one scripted response.

  std::vector<uint8_t> data(kMaxWriteRequestBytes + 10, 0xCD);  // 2 chunks.
  auto r = WriteWholeFile(&c, kTarget, data);
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.write_calls, 1);  // second chunk never attempted.
  EXPECT_EQ(c.close_calls, 0);  // poisoned session never closed.
}

TEST(EfsWriter, AmbiguousParserFailureDoesNotReuseWriter) {
  // OPEN's response is a clean transport round trip that the parser
  // could not structurally parse (item B/2) - ambiguous, since the
  // truncate side effect may already have happened. No WRITE call may
  // ever be attempted against this (possibly poisoned, and in any case
  // fd-less) session.
  FakeEfsWriteClient c;
  MutationOpenResult malformed_open;
  malformed_open.transport_status = TransportStatus::kOk;
  malformed_open.parsed.ok = false;
  c.open_response = malformed_open;

  auto r = WriteWholeFile(&c, kTarget, {0x01, 0x02, 0x03});
  EXPECT_EQ(r.state, EfsWriteFileState::kFailed);
  EXPECT_TRUE(r.mutation_outcome_ambiguous);
  EXPECT_EQ(c.write_calls, 0);  // writer never reused/invoked.
  EXPECT_EQ(c.close_calls, 0);
}
