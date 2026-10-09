#pragma once
#include <string>
#include "carrier_catalog.h"
#include <vector>
#include "process_efs_executor.h"

namespace guacamole_efs::w1 {
inline constexpr const char* kSourceHash=guacamole_catalog::kStockHash;
inline constexpr const char* kTargetHash=guacamole_catalog::kKoreaHash;
inline constexpr size_t kSourceSize=guacamole_catalog::kStockSize,kTargetSize=guacamole_catalog::kKoreaSize;
inline constexpr char kContext[] = "u:r:guacamole_efs_experiment:s0";
inline constexpr char kApplyConfirmation[] = "GUACAMOLE_KOREA_SEL_DB_WRITE_EXPERIMENT_V1";
inline constexpr char kRollbackConfirmation[] = "GUACAMOLE_KOREA_SEL_DB_ROLLBACK_V1";
inline constexpr char kAutoEvidence[] =
    "GUACAMOLE_AUTO_SLOT0_EXPERIMENT_V1\nstate=REVIEW_REQUIRED\nmessage=0x2e\n"
    "wire=010400000000001104000300000013010001\n";
enum class Command { kInvalid, kInspect, kApply, kRollback };
enum class DbState { kStock, kKorea, kUnknown };
Command ParseCommand(const std::vector<std::string>& args);
std::string Hash(const std::vector<uint8_t>& bytes);
DbState Classify(const std::vector<uint8_t>& bytes);
const char* Name(DbState state);
std::string Pending(bool rollback);
std::string Verified(bool rollback);
std::string Entered(bool rollback);
struct Preconditions {
  bool root=false, debug=false, context=false, auto_evidence=false;
  bool slot0=false, slot1=false, common_idle=false, profiles=false;
  bool artifact=false, journal=false;
  bool active_known=false, pending_none=false;
  bool Allowed() const;  // Apply only: Common/NONE is mandatory.
  bool RollbackAllowed() const;
};
struct Observation {
  bool complete=false;
  std::vector<uint8_t> bytes;
  std::string error;
};
struct Report {
  std::string outcome="BLOCKED", detail;
  DbState before=DbState::kUnknown, after=DbState::kUnknown;
  bool attempted=false, pending=false;
  int exit_code=4;
};
// One fixed transaction only. No path, payload, carrier, feature-write API.
// IO implementations MUST return only after their worker has been reaped.
class IO {
 public:
  virtual ~IO()=default;
  virtual Preconditions Preflight()=0;
  virtual bool HasApplyPending()=0;
  virtual bool HasRollbackPending()=0;
  virtual Observation ReadFresh()=0;
  virtual bool SaveBefore(const std::vector<uint8_t>& bytes)=0;
  virtual bool BackupValid()=0;
  virtual bool Checkpoint(bool rollback)=0;
  virtual ProcessResult WriteFresh(bool rollback)=0;
  virtual bool RecordVerified(bool rollback)=0;
};
Report RunExperiment(Command command, IO& io);
// Shared durability state machine. Caller must complete its own authority/radio
// preflight. Debug uses RunExperiment; production uses authenticated coordinator.
Report RunDurableTransaction(Command command, IO& io);
}
