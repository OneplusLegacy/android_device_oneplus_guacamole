#pragma once
#include "w1_journal.h"
namespace guacamole_efs::w1 {
inline constexpr char kExecutable[]="/vendor/bin/guacamole_efs_experiment";
inline constexpr char kWorkerCommand[]="--w1-owned-worker";
inline constexpr char kJournalDir[]="/data/vendor/guacamole_efs_experiment";
inline constexpr char kReportsDir[]="/data/vendor/guacamole_efs_reports";
inline constexpr char kJournalContext[]="u:object_r:guacamole_efs_experiment_data_file:s0";
std::string SelfContext();
bool EnvironmentAllowed();
int OpenPrivateDirectory(const char* path,const char* context);
bool AutoEvidenceVerified();
bool JournalFileLabel(int fd);
class W1Launcher final:public IProcessWorkerLauncher {
 public:
  explicit W1Launcher(Journal& journal):journal_(journal){}
  ProcessResult Run(const ProcessRequest&) override;
 private:Journal& journal_;
};
int WorkerMain(int argc,char** argv);
}
