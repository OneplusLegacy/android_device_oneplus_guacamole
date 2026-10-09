#include "production_core.h"
#include <time.h>
namespace guacamole_production {
bool AuthorizedController(const Caller& c){
 // Kernel-supplied SID is unique to the platform-signed, non-shared-UID owner-user APK.
 // Numeric app UID is assigned by PackageManager; root/system/shell/isolated/secondary users fail.
 return c.uid>=10000&&c.uid<=19999&&c.sid=="u:r:guacamole_efs_controller:s0";
}
int64_t ElapsedMs(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)==0?int64_t(t.tv_sec)*1000+t.tv_nsec/1000000:0;}
std::string Recovery(guacamole_efs::w1::DbState live,bool valid,bool pending,bool verified,bool rollback,const LegacyEvidence& legacy,bool rollback_verified){
 using guacamole_efs::w1::DbState;
 if(!valid||!legacy.available||!legacy.valid||live==DbState::kUnknown)return "HARD_STOP";
 if(rollback)return rollback_verified&&live==DbState::kStock?"RESTORED_VERIFIED_PRODUCTION":"ROLLBACK_REVIEW";
 if(pending)return verified&&live==DbState::kKorea?"INSTALLED_VERIFIED_PRODUCTION":"RECOVERY_REQUIRED";
 if(legacy.rollback)return "LEGACY_ROLLBACK_REVIEW";
 if(legacy.pending)return legacy.InstalledHistory()&&live==DbState::kKorea?"INSTALLED_VERIFIED_HISTORY":"RECOVERY_REQUIRED";
 return live==DbState::kKorea?"INSTALLED_NO_CAPTURED_HISTORY":"NO_ATTEMPT";
}
Disposition Decide(Action action,guacamole_efs::w1::DbState live,const Radio& radio,
 const LegacyEvidence& legacy,bool valid,bool pending,bool rollback_pending,bool backup){
 using guacamole_efs::w1::DbState;
 if(action==Action::Observe)return Disposition::ObserveOnly;
 if(live==DbState::kUnknown)return Disposition::Block;
 if((action==Action::Install&&live==DbState::kKorea)||(action==Action::Rollback&&live==DbState::kStock))return Disposition::Noop;
 if(!valid||!legacy.available||!legacy.valid||!legacy.auto_valid)return Disposition::Block;
 if(action==Action::Install)return !pending&&!legacy.pending&&!rollback_pending&&radio.InstallAllowed()?Disposition::Install:Disposition::Block;
 if(action==Action::Rollback)return !rollback_pending&&!legacy.rollback&&radio.RollbackAllowed()&&((pending&&backup)||(legacy.pending&&legacy.backup&&legacy.auto_exact))?Disposition::Rollback:Disposition::Block;
 return Disposition::Block;
}
Coordinator::Coordinator(Backend b):backend_(std::move(b)){thread_=std::thread([this]{Loop();});}
Coordinator::~Coordinator(){{std::lock_guard<std::mutex> g(lock_);stop_=true;}cv_.notify_one();thread_.join();}
Snapshot Coordinator::Observe(){std::lock_guard<std::mutex> g(lock_);
 if(!queued_&&!running_&&(snapshot_.elapsed_ms==0||ElapsedMs()-snapshot_.elapsed_ms>30000)){queued_=true;action_=Action::Observe;cv_.notify_one();}
 auto s=snapshot_;s.busy=running_||queued_;
 if(s.elapsed_ms&&ElapsedMs()-s.elapsed_ms>30000){s.radio=Radio{};s.xml="UNKNOWN";s.recovery="STALE_OBSERVATION";s.install_candidate=false;s.rollback_candidate=false;}
 return s;
}
bool Coordinator::Submit(Action a,const Caller& caller){
 // Authorization BEFORE lock, scheduling, identity clearing, journal or radio effects.
 if((a!=Action::Install&&a!=Action::Rollback)||!AuthorizedController(caller))return false;
 std::lock_guard<std::mutex> g(lock_);if(queued_||running_)return false;
 action_=a;queued_=true;snapshot_.install_candidate=false;snapshot_.rollback_candidate=false;cv_.notify_one();return true;
}
void Coordinator::Loop(){for(;;){
 Action action;
 {std::unique_lock<std::mutex> g(lock_);cv_.wait(g,[&]{return stop_||queued_;});if(stop_)return;action=action_;queued_=false;running_=true;}
 const auto started=ElapsedMs();
 auto result=backend_(action);result.elapsed_ms=started;
 {std::lock_guard<std::mutex> g(lock_);result.generation=snapshot_.generation+1;result.busy=false;snapshot_=std::move(result);running_=false;}
}}
}
