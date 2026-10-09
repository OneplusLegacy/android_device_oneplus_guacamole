#include "production_core.h"
#include "production_worker.h"
#include "w1_artifact.h"
namespace guacamole_production {
using namespace guacamole_efs;
using namespace guacamole_efs::w1;
namespace {
class ProductionIO final:public IO {
 public:
  explicit ProductionIO(Journal& j):journal(j),launcher(j),executor(launcher){}
  Preconditions Preflight() override {return {}; /* Debug-only entry is never used. */}
  bool HasApplyPending() override{return journal.HasOrigin();}
  bool HasRollbackPending() override{return journal.Has("rollback.pending");}
  Observation ReadFresh() override {
   const auto r=executor.ReadTarget(EfsTarget::kMcfgSelDb,"production-read-"+std::to_string(++sequence));
   Observation o;o.complete=r.spawned&&r.reaped&&!r.timed_out&&r.exit_code==0&&r.signal==0&&r.report_valid&&r.error.empty()&&r.report.outcome==ProcessOutcome::kConfirmed&&r.report.teardown_completed;
   if(o.complete)o.bytes=r.report.bytes;
   if(strict_baseline){strict_baseline=false;if(o.complete&&Classify(o.bytes)==DbState::kUnknown){o.complete=false;o.error="UNKNOWN fresh baseline";}}
   if(o.error.empty())o.error=r.error;return o;
  }
  bool BackupValid() override {std::vector<uint8_t>b;return journal.Backup(&b);}
  bool SaveBefore(const std::vector<uint8_t>& b) override{return journal.SaveBefore(b);}
  bool Checkpoint(bool rollback) override{
   if(!CoordinatorIdentity())return false;
   const auto now=ReadRadio();const auto evidence=RequestLegacyEvidence(false);
   if(!evidence.available||!evidence.valid||!evidence.auto_valid||(journal.Has("legacy.origin")&&!evidence.auto_exact))return false;
   if(!(rollback?now.RollbackAllowed():now.InstallAllowed()))return false;
   return journal.Checkpoint(rollback);
  }
  void RequireExactBaseline(){strict_baseline=true;}
  ProcessResult WriteFresh(bool rollback) override{
   std::vector<uint8_t>b;if(rollback){if(!journal.Backup(&b))return {};}else b=PackagedProposal();
   return executor.WriteTarget(EfsWriteTarget::kMcfgSelDb,b,rollback?"rollback":"apply");
  }
  bool RecordVerified(bool rollback) override{return journal.Verify(rollback);}
 private:Journal& journal;ProductionLauncher launcher;ProcessEfsExecutor executor;unsigned sequence=0;bool strict_baseline=false;
};
}
Snapshot Execute(Action action){
 Snapshot s;s.last_action=action==Action::Observe?"OBSERVE":action==Action::Install?"INSTALL":"ROLLBACK";
 if(!CoordinatorIdentity()){s.result="WRONG_COORDINATOR_IDENTITY";return s;}
 // Independent field failure: still observe radio if journal/evidence is unavailable.
 s.radio=ReadRadio();auto legacy=RequestLegacyEvidence(false);
 Journal journal(OpenDirectory(kDir,kDaemonUid,kDataLabel),kDaemonUid,ProductionFileLabel,true);
 if(!journal.Lock()){s.recovery="HARD_STOP";s.result="JOURNAL_INVALID_OR_BUSY";return s;}
 ProductionIO io(journal);
 const auto current=io.ReadFresh();auto live=current.complete?Classify(current.bytes):DbState::kUnknown;
 s.xml=Name(live);
 s.recovery=Recovery(live,journal.Validate(),journal.HasOrigin(),journal.Has("korea_sel_db.verified"),journal.Has("rollback.pending"),legacy,journal.Has("rollback.verified")&&journal.Has("rollback.worker"));
 const bool saved=io.BackupValid();
 auto decide=[&](Action a){return Decide(a,live,s.radio,legacy,journal.Validate(),journal.HasOrigin(),journal.Has("rollback.pending"),saved);};
 s.install_candidate=decide(Action::Install)==Disposition::Install;
 s.rollback_candidate=decide(Action::Rollback)==Disposition::Rollback;
 if(action==Action::Observe){s.result="OBSERVED";return s;}
 if(Classify(PackagedProposal())!=DbState::kKorea||Classify(PackagedOriginal())!=DbState::kStock){s.result="ARTIFACT_INVALID";return s;}
 if(!current.complete||live==DbState::kUnknown){s.result="HARD_STOP_UNKNOWN";return s;}
 // Correct state is a read-only NOOP even when auto selection is SKT and pending evidence is permanent.
 if(action==Action::Install&&live==DbState::kKorea){s.result="ALREADY_TARGET_NOOP";return s;}
 if(action==Action::Rollback&&live==DbState::kStock){s.result="ALREADY_ORIGINAL_NOOP";return s;}
 if(action==Action::Install&&!s.install_candidate){s.result="INSTALL_PREFLIGHT_BLOCKED";return s;}
 if(action==Action::Rollback){
  if(!s.rollback_candidate){s.result="ROLLBACK_PREFLIGHT_BLOCKED";return s;}
  if(!journal.HasOrigin()){
   // Only this explicit authorized recovery invocation asks the legacy authority for captured bytes.
   const auto backup=RequestLegacyEvidence(true);
   if(!backup.RecoveryBackup()||!journal.ImportLegacy(backup.before)){s.result="LEGACY_BACKUP_UNAVAILABLE";return s;}
  }
 }
 // W1 has a second fresh baseline, durable backup/checkpoint, one worker and fresh readback.
 io.RequireExactBaseline();
 const auto report=RunDurableTransaction(action==Action::Install?Command::kApply:Command::kRollback,io);
 s.result=report.outcome+":"+report.detail;s.xml=Name(report.after);s.recovery="REVIEW_REQUIRED";
 s.install_candidate=false;s.rollback_candidate=false;
 // Never republish radio observed before a mutation as new post-mutation evidence.
 if(report.pending){s.radio=Radio{};}
 return s;
}
}
