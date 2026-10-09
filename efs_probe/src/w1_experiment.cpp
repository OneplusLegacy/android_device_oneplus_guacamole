#include "w1_experiment.h"
#include "hash.h"
namespace guacamole_efs::w1 {
Command ParseCommand(const std::vector<std::string>& a) {
  if(a==std::vector<std::string>{"inspect-korea-sel-db-readonly"}) return Command::kInspect;
  if(a==std::vector<std::string>{"apply-korea-sel-db-once","--confirm",kApplyConfirmation}) return Command::kApply;
  if(a==std::vector<std::string>{"rollback-korea-sel-db-once","--confirm",kRollbackConfirmation}) return Command::kRollback;
  return Command::kInvalid;
}
std::string Hash(const std::vector<uint8_t>& bytes) {
  const auto h=Sha256(bytes.data(),bytes.size()); std::string out;
  for(auto b:h) {out.push_back("0123456789abcdef"[b>>4]);out.push_back("0123456789abcdef"[b&15]);}
  return out;
}
DbState Classify(const std::vector<uint8_t>& bytes) {
  if(bytes.size()==kSourceSize && Hash(bytes)==kSourceHash) return DbState::kStock;
  if(bytes.size()==kTargetSize && Hash(bytes)==kTargetHash) return DbState::kKorea;
  return DbState::kUnknown;
}
const char* Name(DbState s) {switch(s) {case DbState::kStock:return "STOCK_EXACT";case DbState::kKorea:return "KOREA_EXACT";default:return "UNKNOWN";}}
bool Preconditions::Allowed() const {
  return root&&debug&&context&&auto_evidence&&slot0&&slot1&&common_idle&&profiles&&artifact&&journal;
}
bool Preconditions::RollbackAllowed() const {
  return root&&debug&&context&&auto_evidence&&slot0&&slot1&&active_known&&pending_none&&profiles&&artifact&&journal;
}
std::string Pending(bool rollback) {
  return std::string("GUACAMOLE_KOREA_SEL_DB_EXPERIMENT_V1\noperation=")+(rollback?"rollback":"apply")+
      "\nstate=REVIEW_REQUIRED\nsource_hash="+kSourceHash+"\ntarget_hash="+kTargetHash+
      "\nsource_size=57655\ntarget_size=58941\npath=/nv/item_files/mcfg/mcfg_sel_db.xml\n";
}
std::string Verified(bool rollback) {return Pending(rollback)+"fresh_readback="+(rollback?kSourceHash:kTargetHash)+"\n";}
std::string Entered(bool rollback) {return Pending(rollback)+"worker_entered=true\n";}
Report RunExperiment(Command c,IO& io) {
  Report r;
  if(c==Command::kInvalid) {r.detail="invalid command/confirmation";return r;}
  const auto pre=io.Preflight();
  const bool inspect_safe=pre.root&&pre.debug&&pre.context&&pre.auto_evidence&&pre.artifact&&pre.journal;
  const bool allowed=c==Command::kInspect ? inspect_safe :
      c==Command::kRollback ? pre.RollbackAllowed() : pre.Allowed();
  if(!allowed) {r.detail="preflight failed; no write";return r;}
  return RunDurableTransaction(c,io);
}
Report RunDurableTransaction(Command c,IO& io) {
  Report r;
  if(c!=Command::kApply&&c!=Command::kRollback&&c!=Command::kInspect){r.detail="invalid operation";return r;}
  r.pending=io.HasApplyPending();
  if(c==Command::kApply && r.pending) {r.detail="permanent apply pending blocks every subsequent apply";return r;}
  if(c==Command::kRollback && (!r.pending||io.HasRollbackPending()||!io.BackupValid())) {
    r.detail="rollback requires original pending and exact backup; no previous rollback";return r;
  }
  const auto baseline=io.ReadFresh();
  if(!baseline.complete) {r.outcome="REVIEW_REQUIRED";r.detail="fresh baseline read failed";return r;}
  r.before=Classify(baseline.bytes);
  if(c==Command::kInspect) {
    const auto second=io.ReadFresh();
    if(!second.complete || second.bytes!=baseline.bytes) {r.outcome="REVIEW_REQUIRED";r.detail="independent inspect reads failed/differ";return r;}
    r.after=Classify(second.bytes);r.outcome=r.pending?"REVIEW_REQUIRED":"READ_ONLY";
    r.detail="two fresh workers reaped; exact bytes equal";r.exit_code=0;return r;
  }
  const bool rollback=c==Command::kRollback;
  if(!rollback && r.before==DbState::kKorea) {r.outcome="ALREADY_TARGET_REVIEW";r.exit_code=0;return r;}
  if(!rollback && r.before!=DbState::kStock) {r.outcome="UNKNOWN_BASELINE";return r;}
  if(rollback && r.before==DbState::kStock) {r.outcome="ALREADY_ORIGINAL_REVIEW";r.exit_code=0;return r;}
  // A successful full read is mandatory even for explicit rollback. Read failure
  // never becomes a rollback decision. Rollback requires its own invocation.
  if(!rollback && !io.SaveBefore(baseline.bytes)) {r.detail="backup durability/reread failed";return r;}
  if(!io.Checkpoint(rollback)) {r.detail="checkpoint failed; no write";return r;}
  r.pending=true;
  const auto write=io.WriteFresh(rollback); // EXACTLY ONE logical write, no retry.
  r.attempted=write.spawned; r.exit_code=10; r.outcome="REVIEW_REQUIRED";
  if(write.spawned && !write.reaped) {r.detail="HARD_STOP: worker not reaped; no overlap";return r;}
  // Even a corrupt report or timeout after a reaped worker requires independent
  // observation. No interpretation of send failure as proof of no mutation.
  const auto readback=io.ReadFresh();
  if(!readback.complete) {r.detail="HARD_STOP: fresh readback failed; no automatic rollback";return r;}
  r.after=Classify(readback.bytes);
  const bool exact=r.after==(rollback?DbState::kStock:DbState::kKorea);
  if(exact && io.RecordVerified(rollback)) {r.outcome="VERIFIED_REVIEW_REQUIRED";r.detail="exact fresh-process readback; production remains blocked";}
  else r.detail=exact?"exact readback but verified journal failed":"readback mismatch/unchanged; no retry or automatic rollback";
  return r;
}
}
