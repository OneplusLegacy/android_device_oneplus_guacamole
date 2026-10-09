#include "efs_transaction_backend.h"
#include "hash.h"
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <iterator>

using namespace guacamole_efs;
namespace {
class JournalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char name[] = "/tmp/guacamole-journal-XXXXXX";
    char* p = mkdtemp(name);
    ASSERT_NE(p, nullptr);
    root = p;
    backend = std::make_unique<FileTransactionBackend>(root);
    record.transaction_id = "unique-id";
    record.target = TargetPath(EfsWriteTarget::kMcfgSelDb);
    record.timestamp = 123;
    record.preimage_sha256 = Sha256(bytes.data(), bytes.size());
    record.proposed_sha256.fill(0xab);
    record.verification_result = "pending\nwith \\ escape";
  }
  void TearDown() override { if (!root.empty()) std::filesystem::remove_all(root); }
  void Begin() {
    ASSERT_EQ(backend->PersistPreimageBackup(record.transaction_id, bytes).state,
        BackupPersistState::kOk);
    ASSERT_EQ(backend->RecordPending(record).state, PendingRecordState::kOk);
  }
  std::string Path(const std::string& suffix) {
    auto backup = backend->BackupPathForTesting(record.transaction_id);
    auto name = std::filesystem::path(backup).filename().string();
    return root + "/transactions/" + name.substr(9,32) + suffix;
  }
  void Put(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
    ASSERT_TRUE(out.good());
  }
  std::string Get(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string root;
  std::unique_ptr<FileTransactionBackend> backend;
  TransactionRecord record;
  std::vector<uint8_t> bytes{1,2,3,4};
};
}
TEST_F(JournalTest, EmptyJournalAllowsNewTransaction) {
  EXPECT_TRUE(backend->LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, DurableBackupAndPendingRoundTripAfterRestart) {
  Begin();
  FileTransactionBackend reopened(root);
  auto scan = reopened.LoadPendingTransactions();
  ASSERT_TRUE(scan.ok);
  ASSERT_EQ(scan.records.size(), 1u);
  EXPECT_EQ(scan.records[0].verification_result, record.verification_result);
  ASSERT_TRUE(reopened.LoadBackup(record.transaction_id));
  EXPECT_EQ(*reopened.LoadBackup(record.transaction_id), bytes);
}
TEST_F(JournalTest, OnlyVerifiedResolvedFinalHidesPending) {
  Begin();
  record.state = TransactionState::kSucceeded;
  ASSERT_TRUE(backend->RecordTransaction(record));
  FileTransactionBackend reopened(root);
  EXPECT_TRUE(reopened.LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, UnknownFinalStillNeedsRecovery) {
  Begin();
  ASSERT_TRUE(backend->RecordTransaction(record));
  FileTransactionBackend reopened(root);
  EXPECT_EQ(reopened.LoadPendingTransactions().records.size(), 1u);
}
TEST_F(JournalTest, HardStopFinalRemainsHardStopAfterRestart) {
  Begin();
  record.state = TransactionState::kRollbackFailedHardStop;
  ASSERT_TRUE(backend->RecordTransaction(record));
  FileTransactionBackend reopened(root);
  auto scan = reopened.LoadPendingTransactions();
  ASSERT_EQ(scan.records.size(), 1u);
  EXPECT_EQ(scan.records[0].state, TransactionState::kRollbackFailedHardStop);
}
TEST_F(JournalTest, EmptyFinalDoesNotHidePending) {
  Begin(); Put(Path(".final"), "");
  FileTransactionBackend reopened(root);
  EXPECT_EQ(reopened.LoadPendingTransactions().records.size(), 1u);
}
TEST_F(JournalTest, TornFinalDoesNotHidePending) {
  Begin(); record.state = TransactionState::kSucceeded;
  ASSERT_TRUE(backend->RecordTransaction(record));
  auto data = Get(Path(".final"));
  Put(Path(".final"), data.substr(0, data.size()/2));
  FileTransactionBackend reopened(root);
  EXPECT_EQ(reopened.LoadPendingTransactions().records.size(), 1u);
}
TEST_F(JournalTest, FinalChecksumMismatchDoesNotHidePending) {
  Begin(); record.state = TransactionState::kSucceeded;
  ASSERT_TRUE(backend->RecordTransaction(record));
  auto data = Get(Path(".final"));
  data[data.find("timestamp=")+10] = '9';
  Put(Path(".final"), data);
  FileTransactionBackend reopened(root);
  EXPECT_FALSE(reopened.LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, DuplicateOrInvalidPendingFailsClosed) {
  Begin();
  Put(Path(".pending"), Get(Path(".pending")) + "state=4\n");
  FileTransactionBackend reopened(root);
  EXPECT_FALSE(reopened.LoadPendingTransactions().ok);
}
TEST_F(JournalTest, LegacyUnchecksummedRecordFailsClosed) {
  Put(Path(".pending"), "transaction_id=unique-id\nstate=4\n");
  EXPECT_FALSE(backend->LoadPendingTransactions().ok);
}
TEST_F(JournalTest, OrphanBackupAfterCheckpointCrashBlocksMutation) {
  ASSERT_EQ(backend->PersistPreimageBackup(record.transaction_id, bytes).state,
      BackupPersistState::kOk);
  FileTransactionBackend reopened(root);
  EXPECT_FALSE(reopened.LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, TransactionIdCannotOverwriteOriginalBackup) {
  Begin();
  EXPECT_NE(backend->PersistPreimageBackup(record.transaction_id, {9}).state,
      BackupPersistState::kOk);
  FileTransactionBackend reopened(root);
  ASSERT_TRUE(reopened.LoadBackup(record.transaction_id));
  EXPECT_EQ(*reopened.LoadBackup(record.transaction_id), bytes);
}
TEST_F(JournalTest, DuplicatePendingAndFinalAreRejected) {
  Begin();
  auto pending = Get(Path(".pending"));
  EXPECT_NE(backend->RecordPending(record).state, PendingRecordState::kOk);
  EXPECT_EQ(Get(Path(".pending")), pending);
}
TEST_F(JournalTest, FinalIdentityMustMatchPending) {
  Begin(); record.timestamp++;
  EXPECT_FALSE(backend->RecordTransaction(record));
  EXPECT_FALSE(backend->LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, MissingOrCorruptBackupCannotCompleteTransaction) {
  Begin(); record.state = TransactionState::kSucceeded;
  ASSERT_TRUE(backend->RecordTransaction(record));
  Put(backend->BackupPathForTesting(record.transaction_id), "corrupt");
  FileTransactionBackend reopened(root);
  EXPECT_FALSE(reopened.LoadPendingTransactions().ok);
  EXPECT_FALSE(reopened.LoadBackup(record.transaction_id));
}
TEST_F(JournalTest, JournalDirectoryReadFailureIsNotEmptySuccess) {
  std::filesystem::remove_all(root + "/transactions");
  Put(root + "/transactions", "not a directory");
  EXPECT_FALSE(backend->LoadPendingTransactions().ok);
}
TEST_F(JournalTest, SymlinkFinalCannotHidePending) {
  Begin();
  ASSERT_EQ(symlink(Path(".pending").c_str(), Path(".final").c_str()), 0);
  EXPECT_FALSE(backend->LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(JournalTest, RootMustAlreadyBeProvisioned) {
  FileTransactionBackend invalid(root + "/missing");
  EXPECT_FALSE(invalid.LoadPendingTransactions().ok);
  EXPECT_NE(invalid.PersistPreimageBackup("id", bytes).state, BackupPersistState::kOk);
}
TEST_F(JournalTest, HostileIdCannotEscapeDirectory) {
  auto path = backend->BackupPathForTesting("../../outside");
  EXPECT_EQ(path.find(".."), std::string::npos);
  EXPECT_EQ(path.find(root + "/backup/"), 0u);
}
