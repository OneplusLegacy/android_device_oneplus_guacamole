#pragma once
#include "production_radio.h"
#include "legacy_evidence.h"
#include "w1_experiment.h"
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
namespace guacamole_production {
struct Caller {unsigned uid=0;std::string sid;};
bool AuthorizedController(const Caller& caller);
enum class Action { Observe, Install, Rollback };
enum class Disposition { ObserveOnly, Noop, Block, Install, Rollback };
Disposition Decide(Action action,guacamole_efs::w1::DbState live,const Radio& radio,
 const LegacyEvidence& legacy,bool journal_valid,bool pending,bool rollback_pending,bool backup);
struct Snapshot {
 Radio radio;
 std::string xml="UNKNOWN",recovery="UNOBSERVED",last_action="NONE",result="UNOBSERVED";
 int64_t generation=0,elapsed_ms=0;
 bool busy=false,install_candidate=false,rollback_candidate=false;
};
std::string Recovery(guacamole_efs::w1::DbState live,bool local_valid,bool local_pending,bool local_verified,bool local_rollback,const LegacyEvidence& legacy,bool rollback_verified=false);
// Injectable one serialized backend. Accepted mutation does not imply success.
class Coordinator {
 public:
  using Backend=std::function<Snapshot(Action)>;
  explicit Coordinator(Backend backend);
  ~Coordinator();
  Snapshot Observe();
  bool Submit(Action action,const Caller& caller);
 private:
  void Loop();
  Backend backend_;std::mutex lock_;std::condition_variable cv_;std::thread thread_;
  bool stop_=false,queued_=false,running_=false;Action action_=Action::Observe;Snapshot snapshot_;
};
int64_t ElapsedMs();
Snapshot Execute(Action action);
}
