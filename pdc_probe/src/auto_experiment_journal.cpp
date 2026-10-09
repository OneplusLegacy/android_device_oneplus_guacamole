#include "auto_experiment_journal.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace guacamole_pdc {
int OpenDeviceAutoExperimentDirectory() {
  // Fixed init-owned location under system-owned /data/vendor. Do not request
  // parent directory listing permissions or accept any caller-supplied path.
  return open("/data/vendor/guacamole_pdc_experiment",
              O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
}
AutoExperimentJournal::~AutoExperimentJournal() {
  if (lock_>=0) close(lock_);
  if (directory_>=0) close(directory_);
}
bool AutoExperimentJournal::CleanDirectory() const {
  // Independent directory description avoids a shared readdir offset on recheck.
  int fd=openat(directory_,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if (fd<0) return false;
  DIR* dir=fdopendir(fd);
  if (!dir) { close(fd);return false; }
  bool clean=true;
  errno=0;
  while (const auto* e=readdir(dir)) {
    if (std::strcmp(e->d_name,".") && std::strcmp(e->d_name,"..") && std::strcmp(e->d_name,"lock")) {
      clean=false;break; // ANY pending/torn/final/unexpected entry blocks
    }
    errno=0;
  }
  if (errno) clean=false;
  closedir(dir);
  return clean;
}
bool AutoExperimentJournal::AcquireClean() {
  if (acquire_consumed_) return false;
  acquire_consumed_=true;
  struct stat d{},l{},named{};
  if (directory_<0 || fstat(directory_,&d) || !S_ISDIR(d.st_mode) ||
      d.st_uid!=owner_ || (d.st_mode&07777)!=0700) return false;
  lock_=openat(directory_,"lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
  if (lock_<0 || fstat(lock_,&l) || !S_ISREG(l.st_mode) || l.st_uid!=owner_ ||
      l.st_nlink!=1 || (l.st_mode&07777)!=0600 || flock(lock_,LOCK_EX|LOCK_NB) ||
      fstatat(directory_,"lock",&named,AT_SYMLINK_NOFOLLOW) ||
      l.st_ino!=named.st_ino || l.st_dev!=named.st_dev || !CleanDirectory()) return false;
  acquired_=true;
  return true;
}
bool AutoExperimentJournal::RecordPending() {
  if (!acquired_ || record_consumed_) return false;
  record_consumed_=true;
  if (!CleanDirectory()) return false;
  int fd=openat(directory_,"slot0.pending",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if (fd<0) return false;
  constexpr char marker[]="GUACAMOLE_AUTO_SLOT0_EXPERIMENT_V1\nstate=REVIEW_REQUIRED\nmessage=0x2e\nwire=010400000000001104000300000013010001\n";
  size_t offset=0;
  while (offset<sizeof(marker)-1) {
    const auto n=write(fd,marker+offset,sizeof(marker)-1-offset);
    if (n<0 && errno==EINTR) continue; // only local interrupted file I/O, never QMI retry
    if (n<=0) { close(fd);return false; }
    offset+=static_cast<size_t>(n);
  }
  const bool synced=fsync(fd)==0;
  const bool closed=close(fd)==0;
  if (!synced || !closed || fsync(directory_)) return false;
  durable_=true;
  return true; // deliberately never remove/update marker, even on protocol success
}
}
