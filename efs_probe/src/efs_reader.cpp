#include "efs_reader.h"

namespace guacamole_efs {

namespace {
// POSIX S_IFMT/S_IFDIR - same convention qtools' efsio.c uses
// (`S_ISDIR(fi->mode)` in efs_stat()) - PUBLIC_REFERENCE_CONFIRMED.
constexpr int32_t kModeFmtMask = 0170000;
constexpr int32_t kModeDir = 0040000;

EfsReadFileResult Failed(const std::string& message) {
  EfsReadFileResult r;
  r.state = EfsReadFileState::kFailed;
  r.message = message;
  return r;
}
}  // namespace

EfsReadFileResult ReadWholeFile(IEfsClient* client, const std::string& path) {
  EfsStatResult st = client->Stat(path);
  if (!st.ok) {
    return Failed("stat: malformed/short response");
  }
  if (st.diag_errno != 0) {
    return Failed("stat: diag_errno=" + std::to_string(st.diag_errno));
  }
  if ((st.mode & kModeFmtMask) == kModeDir) {
    return Failed("stat: path is a directory, not a file");
  }
  if (st.size < 0) {
    return Failed("stat: negative size reported");
  }
  if (st.size > kMaxFileSizeBytes) {
    return Failed("stat: size " + std::to_string(st.size) +
                   " exceeds this project's own kMaxFileSizeBytes cap (" +
                   std::to_string(kMaxFileSizeBytes) + ") - rejected, not "
                   "truncated");
  }

  EfsReadFileResult result;
  result.stat_size = st.size;

  if (st.size == 0) {
    EfsOpenResult open_r = client->OpenReadOnly(path);
    if (!open_r.ok || open_r.diag_errno != 0 || open_r.fd < 0) {
      return Failed("open: failed on empty file");
    }
    EfsCloseResult close_r = client->Close(open_r.fd);
    if (!close_r.ok || close_r.errno_value != 0) {
      return Failed("close: failed after empty-file read");
    }
    result.state = EfsReadFileState::kDone;
    result.total_bytes_read = 0;
    return result;
  }

  EfsOpenResult open_r = client->OpenReadOnly(path);
  if (!open_r.ok || open_r.diag_errno != 0 || open_r.fd < 0) {
    return Failed("open: failed");
  }
  int32_t fd = open_r.fd;

  uint32_t offset = 0;
  uint32_t remaining = static_cast<uint32_t>(st.size);
  while (remaining > 0) {
    uint32_t chunk = remaining < kMaxReadRequestBytes ? remaining
                                                       : kMaxReadRequestBytes;
    EfsReadResult r = client->Read(fd, chunk, offset);
    if (!r.ok) {
      client->Close(fd);
      return Failed("read: malformed/mismatched response at offset " +
                     std::to_string(offset));
    }
    if (r.diag_errno != 0) {
      client->Close(fd);
      return Failed("read: diag_errno=" + std::to_string(r.diag_errno) +
                     " at offset " + std::to_string(offset));
    }
    if (r.bytes_read <= 0) {
      client->Close(fd);
      return Failed("read: zero/negative bytes_read before reaching "
                     "expected size (offset " +
                     std::to_string(offset) + ")");
    }
    // Do not trust the client's bytes_read implicitly - the REAL client
    // is backed by ParseReadResponse's own bytes_read<=requested<=chunk
    // invariant, but IEfsClient is an interface any implementation
    // (including a test fake, or a future bug) could violate. A
    // bytes_read exceeding what was actually requested would make
    // `remaining -= bytes_read` wrap around (remaining is unsigned),
    // corrupting every subsequent offset/remaining computation and
    // potentially looping far past the real end of the file. Checked
    // explicitly, not assumed.
    if (static_cast<uint32_t>(r.bytes_read) > chunk) {
      client->Close(fd);
      return Failed("read: bytes_read (" + std::to_string(r.bytes_read) +
                     ") exceeds requested chunk size (" +
                     std::to_string(chunk) + ") at offset " +
                     std::to_string(offset));
    }
    result.data.insert(result.data.end(), r.data.begin(), r.data.end());
    offset += static_cast<uint32_t>(r.bytes_read);
    remaining -= static_cast<uint32_t>(r.bytes_read);
  }

  EfsCloseResult close_r = client->Close(fd);
  if (!close_r.ok || close_r.errno_value != 0) {
    return Failed("close: failed after a fully-successful read");
  }

  result.total_bytes_read = offset;
  if (result.total_bytes_read != static_cast<uint32_t>(st.size)) {
    return Failed("total bytes read (" +
                   std::to_string(result.total_bytes_read) +
                   ") != stat size (" + std::to_string(st.size) + ")");
  }

  result.state = EfsReadFileState::kDone;
  return result;
}

}  // namespace guacamole_efs
