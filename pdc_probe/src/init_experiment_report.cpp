#include "init_experiment_report.h"
#include "auto_slot0_experiment.h"
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
namespace guacamole_pdc {
std::optional<InitReportCommand> ParseInitReportCommand(const std::vector<std::string>& args) {
  if (args.empty() || args.front()!="--init-report") return std::nullopt;
  const std::vector<std::string> inner(args.begin()+1,args.end());
  if (inner==std::vector<std::string>{"self-context-readonly"}) return InitReportCommand{"context",inner};
  if (inner==std::vector<std::string>{"get-auto-feature-readonly","0"}) return InitReportCommand{"feature0",inner};
  if (inner==std::vector<std::string>{"get-auto-feature-readonly","1"}) return InitReportCommand{"feature1",inner};
  if (inner==std::vector<std::string>{"selected"}) return InitReportCommand{"selected",inner};
  if (IsAutoSlot0Command(inner)) return InitReportCommand{"auto0-enable-once",inner};
  return std::nullopt;
}
int RunInitExperimentReport(const InitReportCommand& command,const std::function<int()>& entry) {
  std::vector<std::string> args{"--init-report"};
  args.insert(args.end(),command.args.begin(),command.args.end());
  const auto validated=ParseInitReportCommand(args);
  if (!validated || validated->label!=command.label || !entry || geteuid()!=0) return 4;
  int dir=open("/data/vendor/guacamole_pdc_reports",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if (dir<0) return 4;
  struct stat st{};
  if (fstat(dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&07777)!=0700) {
    close(dir); return 4;
  }
  timespec stamp{};
  if (clock_gettime(CLOCK_REALTIME,&stamp)) { close(dir); return 4; }
  char name[160];
  const int n=std::snprintf(name,sizeof(name),"%s-%lld-%ld-%ld.log",command.label.c_str(),
      static_cast<long long>(stamp.tv_sec),stamp.tv_nsec,static_cast<long>(getpid()));
  if (n<0 || static_cast<size_t>(n)>=sizeof(name)) { close(dir); return 4; }
  // No overwrite, truncation or retry; even name collision blocks before QCCI.
  int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if (fd<0) { close(dir); return 4; }
  const int saved_out=fcntl(STDOUT_FILENO,F_DUPFD_CLOEXEC,3);
  const int saved_err=fcntl(STDERR_FILENO,F_DUPFD_CLOEXEC,3);
  bool ready=saved_out>=0 && saved_err>=0;
  if (ready) ready=std::fflush(stdout)==0 && std::fflush(stderr)==0;
  if (ready) ready=dup2(fd,STDOUT_FILENO)>=0 && dup2(fd,STDERR_FILENO)>=0;
  int result=4;
  if (ready) {
    std::printf("report_command=%s\nreport_pid=%ld\n",command.label.c_str(),static_cast<long>(getpid()));
    ready=std::fflush(stdout)==0 && fsync(fd)==0 && fsync(dir)==0;
    if (ready) {
      result=entry();
      std::printf("command_exit=%d\nreport_complete=true\n",result);
      // A killed process or failed report write never authorizes a retry.
      const bool flushed=std::fflush(stdout)==0 && std::fflush(stderr)==0;
      if (!flushed || fsync(fd)!=0) std::fprintf(stderr,"report_durability=UNKNOWN\n");
    }
  }
  std::fflush(stdout); std::fflush(stderr);
  if (saved_out>=0) { dup2(saved_out,STDOUT_FILENO); close(saved_out); }
  if (saved_err>=0) { dup2(saved_err,STDERR_FILENO); close(saved_err); }
  close(fd); close(dir);
  return result;
}
}
