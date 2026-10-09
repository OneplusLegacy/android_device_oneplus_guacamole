#include "efs_dir_lister.h"

#include <string>

namespace guacamole_efs {

namespace {
ListDirectoryResult Failed(const std::string& msg) {
  ListDirectoryResult r;
  r.state = ListDirectoryState::kFailed;
  r.message = msg;
  return r;
}
}  // namespace

bool IsZeroSentinelEndOfDirectory(const EfsReadDirResult& r) {
  return r.diag_errno == 0 && r.entry_type == 0 && r.mode == 0 &&
         r.size == 0 && r.atime == 0 && r.mtime == 0 && r.ctime == 0 &&
         r.name.empty();
}

ListDirectoryResult ListDirectory(IEfsClient* client, const std::string& path) {
  EfsOpenDirResult open_r = client->OpenDir(path);
  if (!open_r.ok) {
    return Failed("opendir: malformed/short response");
  }
  if (open_r.diag_errno != 0) {
    return Failed("opendir: diag_errno=" + std::to_string(open_r.diag_errno));
  }

  ListDirectoryResult result;
  uint32_t dirp = open_r.dirp;
  int32_t seqno = kFirstReadDirSequence;

  while (true) {
    if (result.entries.size() >= static_cast<size_t>(kMaxDirectoryEntries)) {
      client->CloseDir(dirp);  // best-effort.
      return Failed("readdir: exceeded max entry cap (" +
                     std::to_string(kMaxDirectoryEntries) + ")");
    }

    EfsReadDirResult rd = client->ReadDir(dirp, seqno);
    if (!rd.ok) {
      client->CloseDir(dirp);  // best-effort.
      return Failed("readdir: malformed/short response at seqno " +
                     std::to_string(seqno));
    }
    if (rd.dirp != dirp) {
      client->CloseDir(dirp);  // best-effort.
      return Failed("readdir: response dirp mismatch at seqno " +
                     std::to_string(seqno));
    }
    if (rd.seqno != seqno) {
      // Covers both "non-progressing" (an unrelated/stale seqno) and
      // "duplicate" (a previously-seen seqno) cases - both are the
      // same underlying signal: the response doesn't correspond to
      // what THIS call asked for, so it cannot be trusted as this
      // entry's data. Fail closed rather than silently accept it or
      // loop forever hoping for a matching one.
      client->CloseDir(dirp);  // best-effort.
      return Failed("readdir: non-progressing sequence (requested " +
                     std::to_string(seqno) + ", response reported " +
                     std::to_string(rd.seqno) + ")");
    }
    if (rd.diag_errno == kEfsErrDirEntryNotFound) {
      // The one nonzero errno currently treated as a plausible normal
      // end-of-directory signal - see this file's header comment
      // (PUBLIC_REFERENCE_SUPPORTED, not yet H41_DEVICE_CONFIRMED).
      break;
    }
    if (rd.diag_errno != 0) {
      // Any OTHER nonzero errno is a genuine EFS2 protocol error
      // (invalid sequence, directory not open, invalid directory
      // entry, etc. - see efs_wire.h's kEfsErr* constants) and must
      // fail the whole listing closed, never be silently treated as
      // EOF - see this file's header comment for the false-success
      // scenario this specifically prevents.
      client->CloseDir(dirp);  // best-effort.
      return Failed("readdir: EFS protocol error, diag_errno=" +
                     std::to_string(rd.diag_errno) + " at seqno " +
                     std::to_string(seqno));
    }

    // diag_errno == 0 here - either a real entry, or the H.41
    // zero-metadata EOD sentinel (H41_DEVICE_CONFIRMED - see this
    // file's header comment). Checked BEFORE ever treating this as a
    // real entry - a genuine zero-length-named entry with nonzero
    // metadata would NOT match this and correctly falls through to be
    // added below.
    if (IsZeroSentinelEndOfDirectory(rd)) {
      break;  // normal end of directory - do NOT send seqno+1.
    }

    EfsDirEntry entry;
    entry.entry_type = rd.entry_type;
    entry.mode = rd.mode;
    entry.size = rd.size;
    entry.atime = rd.atime;
    entry.mtime = rd.mtime;
    entry.ctime = rd.ctime;
    entry.name = rd.name;
    result.entries.push_back(std::move(entry));

    seqno++;
  }

  EfsCloseDirResult close_r = client->CloseDir(dirp);
  if (!close_r.ok || close_r.errno_value != 0) {
    return Failed("closedir: failed after a fully-successful listing");
  }

  result.state = ListDirectoryState::kDone;
  return result;
}

}  // namespace guacamole_efs
