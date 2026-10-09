// Immutable, checksummed, durable transaction storage. Root must be provisioned
// by init; no modem I/O and no automatic recovery writes occur in this class.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "efs_transaction.h"

namespace guacamole_efs {
class FileTransactionBackend final : public ITransactionLog {
 public:
  explicit FileTransactionBackend(std::string root);
  BackupPersistResult PersistPreimageBackup(const std::string& id,
      const std::vector<uint8_t>& preimage) override;
  PendingRecordResult RecordPending(const TransactionRecord& record) override;
  bool RecordTransaction(const TransactionRecord& record) override;
  RecoveryScan LoadPendingTransactions() override;
  std::optional<std::vector<uint8_t>> LoadBackup(const std::string& id) override;
  std::string BackupPathForTesting(const std::string& id) const;
 private:
  std::string root_;
  bool ready_ = false;
  bool failed_ = false;
  // Only this instance's successfully persisted backups can advance to pending.
  std::map<std::string, std::array<uint8_t, 32>> reserved_;
  std::map<std::string, TransactionRecord> pending_;
  std::string BackupPath(const std::string& safe_id) const;
  std::string PendingPath(const std::string& safe_id) const;
  std::string FinalPath(const std::string& safe_id) const;
};
}  // namespace guacamole_efs
