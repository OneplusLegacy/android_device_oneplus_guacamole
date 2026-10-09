#include "efs_transaction_backend.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <cerrno>
#include <charconv>
#include <map>
#include <set>
#include <utility>
#include "hash.h"

namespace guacamole_efs {
namespace {
constexpr size_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr size_t kMaxRecordBytes = 64 * 1024;
std::string ToHex(const uint8_t* data, size_t len) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 15];
  }
  return out;
}
std::string Digest(const std::string& bytes) {
  auto hash = Sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  return ToHex(hash.data(), hash.size());
}
std::string SafeId(const std::string& id) { return Digest(id).substr(0, 32); }
bool PrivateDir(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
      st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
}
bool EnsureDir(const std::string& path) {
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
  return PrivateDir(path);
}
bool FsyncDir(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  bool ok = fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  return ok;
}
// Includes unreadable entries and symlinks as "occupied"; never overwrite them.
bool Absent(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) != 0 && errno == ENOENT;
}
std::optional<std::vector<uint8_t>> ReadFile(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return std::nullopt;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      static_cast<uint64_t>(st.st_size) > kMaxFileBytes) {
    close(fd);
    return std::nullopt;
  }
  std::vector<uint8_t> data;
  uint8_t buf[4096];
  bool ok = true;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { ok = false; break; }
    if (n == 0) break;
    if (data.size() + static_cast<size_t>(n) > kMaxFileBytes) { ok = false; break; }
    data.insert(data.end(), buf, buf + n);
  }
  if (close(fd) != 0) ok = false;
  if (!ok) return std::nullopt;
  return data;
}
// Never truncates/replaces old backup, pending, or final. A torn exclusive
// creation remains visible and blocks recovery; it is never silently deleted.
bool WriteExclusive(const std::string& path, const std::vector<uint8_t>& bytes,
    const std::string& directory) {
  if (bytes.size() > kMaxFileBytes) return false;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  size_t offset = 0;
  bool ok = true;
  while (offset < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = false; break; }
    offset += static_cast<size_t>(n);
  }
  if (fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!FsyncDir(directory)) ok = false;
  auto reread = ReadFile(path);
  return ok && reread && *reread == bytes &&
      Sha256(reread->data(), reread->size()) == Sha256(bytes.data(), bytes.size());
}
std::string Escape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}
std::optional<std::string> Unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') { out += in[i]; continue; }
    if (++i == in.size()) return std::nullopt;
    if (in[i] == 'n') out += '\n';
    else if (in[i] == '\\') out += '\\';
    else return std::nullopt;
  }
  return out;
}
std::string Serialize(const TransactionRecord& r) {
  std::string body = "format=2\ntransaction_id=" + Escape(r.transaction_id) +
      "\ntarget=" + Escape(r.target) + "\ntimestamp=" + std::to_string(r.timestamp) +
      "\nstate=" + std::to_string(static_cast<int>(r.state)) +
      "\npreimage_sha256=" + ToHex(r.preimage_sha256.data(), 32) +
      "\nproposed_sha256=" + ToHex(r.proposed_sha256.data(), 32) +
      "\nverification_result=" + Escape(r.verification_result) +
      "\nrollback_state=" + Escape(r.rollback_state) + "\n";
  return body + "record_sha256=" + Digest(body) + "\n";
}
bool ParseDigest(const std::string& s, std::array<uint8_t, 32>* out) {
  if (s.size() != 64) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; ++i) {
    int a = nibble(s[2*i]), b = nibble(s[2*i+1]);
    if (a < 0 || b < 0) return false;
    (*out)[i] = static_cast<uint8_t>(16*a+b);
  }
  return true;
}
template <typename T> bool Number(const std::string& s, T* result) {
  auto parsed = std::from_chars(s.data(), s.data() + s.size(), *result);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
std::optional<TransactionRecord> Parse(const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > kMaxRecordBytes) return std::nullopt;
  std::string text(bytes.begin(), bytes.end());
  if (text.back() != '\n') return std::nullopt;
  std::map<std::string, std::string> fields;
  for (size_t pos = 0; pos < text.size();) {
    size_t nl = text.find('\n', pos), eq = text.find('=', pos);
    if (nl == std::string::npos || eq == std::string::npos || eq >= nl) return std::nullopt;
    if (!fields.emplace(text.substr(pos, eq-pos), text.substr(eq+1, nl-eq-1)).second)
      return std::nullopt;
    pos = nl+1;
  }
  const char* keys[] = {"format", "transaction_id", "target", "timestamp", "state",
      "preimage_sha256", "proposed_sha256", "verification_result", "rollback_state", "record_sha256"};
  if (fields.size() != 10) return std::nullopt;
  for (auto key : keys) if (!fields.count(key)) return std::nullopt;
  if (fields.at("format") != "2") return std::nullopt;
  auto id = Unescape(fields.at("transaction_id")), target = Unescape(fields.at("target"));
  auto verification = Unescape(fields.at("verification_result")), rollback = Unescape(fields.at("rollback_state"));
  if (!id || id->empty() || id->size() > 1024 || !target || !verification || !rollback)
    return std::nullopt;
  TransactionRecord r;
  r.transaction_id = *id;
  r.target = *target;
  r.verification_result = *verification;
  r.rollback_state = *rollback;
  int state;
  if (!Number(fields.at("timestamp"), &r.timestamp) || r.timestamp < 0 ||
      !Number(fields.at("state"), &state) || state < 0 ||
      state > static_cast<int>(TransactionState::kUnchangedAfterWrite)) return std::nullopt;
  r.state = static_cast<TransactionState>(state);
  if (r.target != TargetPath(EfsWriteTarget::kMcfgSelDb) ||
      !ParseDigest(fields.at("preimage_sha256"), &r.preimage_sha256) ||
      !ParseDigest(fields.at("proposed_sha256"), &r.proposed_sha256)) return std::nullopt;
  // Canonical comparison checks order, duplicates, numeric form, escapes AND checksum.
  if (Serialize(r) != text) return std::nullopt;
  return r;
}
std::optional<TransactionRecord> ReadRecord(const std::string& path) {
  auto bytes = ReadFile(path);
  return bytes ? Parse(*bytes) : std::nullopt;
}
bool SameIdentity(const TransactionRecord& a, const TransactionRecord& b) {
  return a.transaction_id == b.transaction_id && a.target == b.target &&
      a.timestamp == b.timestamp && a.preimage_sha256 == b.preimage_sha256 &&
      a.proposed_sha256 == b.proposed_sha256;
}
bool WriteRecord(const std::string& path, const TransactionRecord& record,
    const std::string& directory) {
  auto text = Serialize(record);
  std::vector<uint8_t> bytes(text.begin(), text.end());
  return Parse(bytes).has_value() && WriteExclusive(path, bytes, directory);
}
bool ListNames(const std::string& path, std::set<std::string>* names) {
  DIR* dir = opendir(path.c_str());
  if (!dir) return false;
  bool ok = true;
  for (;;) {
    errno = 0;
    dirent* e = readdir(dir);
    if (!e) { if (errno != 0) ok = false; break; }
    std::string name(e->d_name);
    if (name != "." && name != "..") names->insert(name);
  }
  if (closedir(dir) != 0) ok = false;
  return ok;
}
bool SafeName(const std::string& s) {
  if (s.size() != 32) return false;
  return s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
}  // namespace

FileTransactionBackend::FileTransactionBackend(std::string root) : root_(std::move(root)) {
  ready_ = PrivateDir(root_) && EnsureDir(root_ + "/backup") &&
      EnsureDir(root_ + "/transactions") && FsyncDir(root_);
}
std::string FileTransactionBackend::BackupPath(const std::string& id) const {
  return root_ + "/backup/preimage-" + id + ".bin";
}
std::string FileTransactionBackend::PendingPath(const std::string& id) const {
  return root_ + "/transactions/" + id + ".pending";
}
std::string FileTransactionBackend::FinalPath(const std::string& id) const {
  return root_ + "/transactions/" + id + ".final";
}
std::string FileTransactionBackend::BackupPathForTesting(const std::string& id) const {
  return BackupPath(SafeId(id));
}
BackupPersistResult FileTransactionBackend::PersistPreimageBackup(const std::string& id,
    const std::vector<uint8_t>& preimage) {
  const auto safe = SafeId(id);
  if (!ready_ || failed_ || id.empty() || id.size() > 1024 ||
      !Absent(PendingPath(safe)) || !Absent(FinalPath(safe)))
    return {BackupPersistState::kPersistFailed, "invalid backend/id or existing transaction"};
  if (!WriteExclusive(BackupPath(safe), preimage, root_ + "/backup")) {
    failed_ = true;
    return {BackupPersistState::kPersistFailed, "backup collision or durable verification failed"};
  }
  reserved_.emplace(id, Sha256(preimage.data(), preimage.size()));
  return {BackupPersistState::kOk, ""};
}
PendingRecordResult FileTransactionBackend::RecordPending(const TransactionRecord& r) {
  auto reserved = reserved_.find(r.transaction_id);
  if (!ready_ || failed_ || reserved == reserved_.end() ||
      reserved->second != r.preimage_sha256 ||
      r.state != TransactionState::kMutationOutcomeUnknown ||
      !Absent(FinalPath(SafeId(r.transaction_id))) ||
      !WriteRecord(PendingPath(SafeId(r.transaction_id)), r, root_ + "/transactions")) {
    failed_ = true;
    return {PendingRecordState::kFailed, "pending collision/invalid record/durability failure"};
  }
  pending_.emplace(r.transaction_id, r);
  return {PendingRecordState::kOk, ""};
}
bool FileTransactionBackend::RecordTransaction(const TransactionRecord& r) {
  auto pending = pending_.find(r.transaction_id);
  if (!ready_ || failed_ || pending == pending_.end() || !SameIdentity(pending->second, r) ||
      !WriteRecord(FinalPath(SafeId(r.transaction_id)), r, root_ + "/transactions")) {
    failed_ = true;
    return false;
  }
  return true;
}
RecoveryScan FileTransactionBackend::LoadPendingTransactions() {
  RecoveryScan scan;
  if (!ready_ || failed_ || !PrivateDir(root_) || !PrivateDir(root_ + "/backup") ||
      !PrivateDir(root_ + "/transactions")) {
    scan.error = "backend unavailable or prior persistence failure";
    return scan;
  }
  std::set<std::string> journals, backups, pending_ids;
  if (!ListNames(root_ + "/transactions", &journals) || !ListNames(root_ + "/backup", &backups)) {
    scan.error = "cannot enumerate journal/backup";
    return scan;
  }
  scan.ok = true;
  auto fail = [&](const std::string& why) { scan.ok = false; scan.error += why + "; "; };
  for (const auto& name : journals) {
    if (name.size() == 40 && name.substr(32) == ".pending" && SafeName(name.substr(0,32)))
      pending_ids.insert(name.substr(0,32));
    else if (!(name.size() == 38 && name.substr(32) == ".final" && SafeName(name.substr(0,32))))
      fail("unexpected journal entry " + name);
  }
  for (const auto& name : journals) {
    if (name.size() == 38 && name.substr(32) == ".final" && !pending_ids.count(name.substr(0,32)))
      fail("orphan final " + name);
  }
  for (const auto& name : backups) {
    if (name.size() != 45 || name.substr(0,9) != "preimage-" || name.substr(41) != ".bin" ||
        !pending_ids.count(name.substr(9,32))) fail("orphan/unknown backup " + name);
  }
  for (const auto& safe : pending_ids) {
    auto pending = ReadRecord(PendingPath(safe));
    if (!pending || SafeId(pending->transaction_id) != safe ||
        pending->state != TransactionState::kMutationOutcomeUnknown) {
      fail("invalid/torn/legacy pending " + safe);
      continue;
    }
    auto backup = ReadFile(BackupPath(safe));
    if (!backup || Sha256(backup->data(), backup->size()) != pending->preimage_sha256) {
      fail("backup missing or corrupt " + safe);
      scan.records.push_back(*pending);
      continue;
    }
    auto final = ReadRecord(FinalPath(safe));
    if (final && SameIdentity(*pending, *final)) {
      if (!IsResolvedTransactionState(final->state)) scan.records.push_back(*final);
    } else {
      // Includes missing, torn, invalid checksum and mismatched final. Pending
      // never disappears simply because a .final directory entry exists.
      scan.records.push_back(*pending);
    }
  }
  return scan;
}
std::optional<std::vector<uint8_t>> FileTransactionBackend::LoadBackup(const std::string& id) {
  if (!ready_ || id.empty()) return std::nullopt;
  auto record = ReadRecord(PendingPath(SafeId(id)));
  auto bytes = ReadFile(BackupPath(SafeId(id)));
  if (!record || record->transaction_id != id || !bytes ||
      Sha256(bytes->data(), bytes->size()) != record->preimage_sha256) return std::nullopt;
  return bytes;
}
}  // namespace guacamole_efs
