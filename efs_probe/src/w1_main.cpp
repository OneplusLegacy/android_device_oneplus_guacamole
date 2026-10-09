#include <cstdio>
#ifndef GUACAMOLE_W1_DEBUG
int main() {std::fputs("EXPERIMENT_BLOCKED: debug build required\n",stderr);return 4;}
#else
#include "w1_device.h"
#include "w1_artifact.h"
#include "w1_status.h"
#include "w1_pdc_readonly.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <iostream>
namespace guacamole_efs::w1 {
namespace {
class DeviceIO final:public IO {
 public:
  explicit DeviceIO(Journal& j):journal_(j),launcher_(j),executor_(launcher_){}
  Preconditions Preflight() override {
    Preconditions p;p.root=geteuid()==0&&getegid()==0;p.context=SelfContext()==kContext;
    p.debug=EnvironmentAllowed();p.journal=journal_.Validate();p.auto_evidence=AutoEvidenceVerified();
    p.artifact=Classify(PackagedProposal())==DbState::kKorea&&Classify(PackagedOriginal())==DbState::kStock;
    if(!p.root||!p.context||!p.debug||!p.journal||!p.auto_evidence||!p.artifact)return p;
    const auto radio=guacamole_pdc::ObserveW1Radio();
    std::cout<<radio.detail;
    p.slot0=radio.slot0;p.slot1=radio.slot1;p.profiles=radio.profiles;p.common_idle=radio.common_idle;
    p.active_known=radio.active_known;p.pending_none=radio.pending_none;
    status_.selected=radio.selected;status_.auto0=radio.auto0;status_.auto1=radio.auto1;status_.profiles=radio.profiles;
    std::cout<<"pre_selected="<<radio.selected<<"\npre_slot0="<<radio.auto0<<"\npre_slot1="<<radio.auto1
      <<"\nprofiles_exact="<<radio.profiles<<"\nauto_evidence_exact="<<p.auto_evidence<<"\npreflight_allowed="<<p.Allowed()<<"\npreflight_rollback_allowed="<<p.RollbackAllowed()
      <<"\nactive_known="<<p.active_known<<"\npending_none="<<p.pending_none<<"\n";
    return p;
  }
  bool HasApplyPending() override{return journal_.Has("korea_sel_db.pending");}
  bool HasRollbackPending() override{return journal_.Has("rollback.pending");}
  Observation ReadFresh() override {
    const auto r=executor_.ReadTarget(EfsTarget::kMcfgSelDb,"read-"+std::to_string(++read_number_));
    PrintWorker(r);
    Observation o;o.complete=r.spawned&&r.reaped&&!r.timed_out&&r.exit_code==0&&r.signal==0&&r.report_valid&&
      r.report.outcome==ProcessOutcome::kConfirmed&&r.report.teardown_completed&&r.error.empty();
    o.error=r.error;if(o.complete)o.bytes=r.report.bytes;
    status_.hash=o.complete&&Classify(o.bytes)!=DbState::kUnknown?Hash(o.bytes):"";
    std::cout<<"live_size="<<o.bytes.size()<<"\nlive_sha256="<<(o.complete?Hash(o.bytes):"UNKNOWN")<<"\n";
    return o;
  }
  bool SaveBefore(const std::vector<uint8_t>& b) override{return journal_.SaveBefore(b);}
  bool BackupValid() override{std::vector<uint8_t> b;return journal_.Backup(&b);}
  bool Checkpoint(bool rollback) override {
    // Recheck immutable evidence/storage immediately before durable checkpoint.
    const bool ok=EnvironmentAllowed()&&AutoEvidenceVerified()&&journal_.Checkpoint(rollback);
    std::cout<<"durable_pending="<<ok<<std::endl;return ok;
  }
  ProcessResult WriteFresh(bool rollback) override {
    // Never republish pre-write evidence as a new post-write observation after
    // an ambiguous/unreaped worker. Only fresh readback may repopulate the hash;
    // radio state needs a separate new inspect invocation after any write launch.
    status_.hash.clear();status_.selected="UNKNOWN";status_.auto0="UNKNOWN";
    status_.auto1="UNKNOWN";status_.profiles=false;
    std::vector<uint8_t> b;
    if(rollback) {if(!journal_.Backup(&b))return {};}
    else b=PackagedProposal();
    auto r=executor_.WriteTarget(EfsWriteTarget::kMcfgSelDb,b,rollback?"rollback":"apply");PrintWorker(r);return r;
  }
  bool RecordVerified(bool rollback) override{return journal_.Verify(rollback);}
  void Publish(const Report& r) {
    status_.valid=true;status_.review=r.pending||r.exit_code!=0||r.outcome!="READ_ONLY"||status_.hash.empty()||!status_.profiles||status_.selected!="Common/NONE";
    std::cout<<"diagnostic_cache_published="<<PublishStatus(status_)<<"\n";
  }
 private:
  static void PrintWorker(const ProcessResult& r) {
    std::cout<<"worker_spawned="<<r.spawned<<" worker_reaped="<<r.reaped<<" worker_exit="<<r.exit_code
      <<" worker_timeout="<<r.timed_out<<" report_valid="<<r.report_valid
      <<" worker_outcome="<<static_cast<int>(r.report.outcome)<<" worker_error="<<r.error<<"\n";
    if(r.report_valid)std::cout<<"worker_nonce="<<WorkerNonceHex(r.report.nonce)<<" teardown="<<r.report.teardown_completed<<"\n";
  }
  Journal& journal_;W1Launcher launcher_;ProcessEfsExecutor executor_;size_t read_number_=0;Status status_;
};
bool OpenReport(Command command) {
  int d=OpenPrivateDirectory(kReportsDir,"u:object_r:guacamole_efs_reports_file:s0");if(d<0)return false;
  timespec t{};if(clock_gettime(CLOCK_REALTIME,&t)!=0){close(d);return false;}
  const std::string name=std::string(command==Command::kInspect?"inspect":command==Command::kApply?"apply":"rollback")+
      "-"+std::to_string(t.tv_sec)+"-"+std::to_string(t.tv_nsec)+"-"+std::to_string(getpid())+".log";
  int fd=openat(d,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if(fd<0){close(d);return false;}
  bool ok=fsync(d)==0&&dup2(fd,STDOUT_FILENO)>=0&&dup2(fd,STDERR_FILENO)>=0;close(fd);close(d);return ok;
}
}
}
int main(int argc,char** argv) {
  using namespace guacamole_efs::w1;
  // No directory/QCCI/DIAG access before actual self-context and build gates.
  if(!EnvironmentAllowed()){std::fputs("EXPERIMENT_BLOCKED: root/debug/exact SELinux execution domain required\n",stderr);return 4;}
  if(argc>1&&std::string(argv[1])==kWorkerCommand)return WorkerMain(argc,argv);
  std::vector<std::string> args;for(int i=1;i<argc;++i)args.emplace_back(argv[i]);
  const auto command=ParseCommand(args);if(command==Command::kInvalid)return 4;
  if(!OpenReport(command))return 4;
  std::cout<<std::boolalpha<<"protocol=W1_V1\nselinux_context="<<SelfContext()<<"\neuid="<<geteuid()<<"\n";
  Journal journal(OpenPrivateDirectory(kJournalDir,kJournalContext),0,JournalFileLabel);
  if(!journal.Lock()){std::cout<<"outcome=BLOCKED_JOURNAL\nexit=4\n"<<std::flush;fsync(1);return 4;}
  DeviceIO io(journal);const auto r=RunExperiment(command,io);io.Publish(r);
  std::cout<<"phase=FINAL_REPORT\nmutation_worker_spawned="<<r.attempted<<"\npending="<<r.pending
    <<"\nbefore="<<Name(r.before)<<"\nafter="<<Name(r.after)<<"\noutcome="<<r.outcome<<"\ndetail="<<r.detail
    <<"\nproduction=BLOCKED_MAPPING_VALIDATION\nexit="<<r.exit_code<<"\n"<<std::flush;
  const bool durable=fsync(1)==0;return durable?r.exit_code:10;
}
#endif
