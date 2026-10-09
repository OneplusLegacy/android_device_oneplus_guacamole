#include "w1_journal.h"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
namespace guacamole_efs::w1 {
namespace {
bool Regular(const struct stat& s,uid_t owner) {
  return S_ISREG(s.st_mode) && s.st_uid==owner && s.st_gid==owner &&
      (s.st_mode&07777)==0600 && s.st_nlink==1 && s.st_size>=0 && s.st_size<=60000;
}
const std::string kLegacyOrigin="GUACAMOLE_LEGACY_CAPTURE_V1\nsource=guacamole_efs_experiment/korea_sel_db.before\napply_pending=validated\nbackup=captured_exact_stock\n";
std::vector<uint8_t> Bytes(const std::string& s) {return {s.begin(),s.end()};}
}
Journal::~Journal() {if(lock_>=0)close(lock_);if(fd_>=0)close(fd_);}
bool Journal::Read(const char* name,std::vector<uint8_t>* bytes,bool durable) const {
  int f=openat(fd_,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if(f<0)return false;
  struct stat st{};bool ok=fstat(f,&st)==0 && Regular(st,owner_) && (!label_||label_(f));
  bytes->clear();
  if(ok) {
    bytes->resize(st.st_size);size_t done=0;
    while(done<bytes->size()) {
      ssize_t n=read(f,bytes->data()+done,bytes->size()-done);
      if(n<0&&errno==EINTR)continue;
      if(n<=0){ok=false;break;}done+=n;
    }
    uint8_t extra; if(ok && read(f,&extra,1)!=0)ok=false;
  }
  // Reused orphan backup may originate from an interrupted fsync phase. Flush
  // this validated read-only descriptor and directory before trusting durability.
  // No byte/owner/mode/name modification is performed.
  if(ok&&durable) {if(fsync(f)!=0)ok=false;if(fsync(fd_)!=0)ok=false;}
  if(close(f)!=0)ok=false;
  return ok;
}
bool Journal::Has(const char* name) const {
  struct stat s{};return fstatat(fd_,name,&s,AT_SYMLINK_NOFOLLOW)==0;
}
bool Journal::Validate() const {
  struct stat d{};
  if(fd_<0||fstat(fd_,&d)!=0||!S_ISDIR(d.st_mode)||d.st_uid!=owner_||d.st_gid!=owner_||(d.st_mode&07777)!=0700)return false;
  // Separate open description: a dup directory fd shares the readdir offset.
  int scan=openat(fd_,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(scan<0)return false;
  DIR* dir=fdopendir(scan);if(!dir){close(scan);return false;}
  const std::map<std::string,std::string> expected{
    {"lock",""},{"korea_sel_db.pending",Pending(false)},
    {"korea_sel_db.verified",Verified(false)},{"apply.worker",Entered(false)},
    {"rollback.pending",Pending(true)},{"rollback.verified",Verified(true)},
    {"rollback.worker",Entered(true)}};
  bool ok=true;errno=0;
  while(auto* e=readdir(dir)) {
    const std::string n=e->d_name;if(n=="."||n=="..")continue;
    std::vector<uint8_t> b;
    if(!Read(n.c_str(),&b)){ok=false;break;}
    if(n=="korea_sel_db.before") {if(Classify(b)!=DbState::kStock){ok=false;break;}}
    else if(n=="legacy.origin") {if(!legacy_||b!=Bytes(kLegacyOrigin)){ok=false;break;}}
    else {
      const auto it=expected.find(n);
      if(it==expected.end()||b!=Bytes(it->second)){ok=false;break;}
    }
    errno=0;
  }
  if(errno!=0)ok=false;
  closedir(dir);
  if((Has("korea_sel_db.pending")||Has("legacy.origin"))&&!Has("korea_sel_db.before"))ok=false;
  if((Has("apply.worker")||Has("korea_sel_db.verified"))&&!Has("korea_sel_db.pending"))ok=false;
  if(Has("rollback.pending")&&!HasOrigin())ok=false;
  if((Has("rollback.worker")||Has("rollback.verified"))&&!Has("rollback.pending"))ok=false;
  return ok;
}
bool Journal::Put(const char* name,const std::vector<uint8_t>& b) {
  // O_EXCL is the durable one-shot boundary. Torn files are retained and block.
  const int f=openat(fd_,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(f<0)return false;
  size_t done=0;bool ok=true;
  while(done<b.size()) {ssize_t n=write(f,b.data()+done,b.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=n;}
  if(fsync(f)!=0)ok=false;
  if(close(f)!=0)ok=false;
  if(fsync(fd_)!=0)ok=false;
  std::vector<uint8_t> reread;
  return ok&&Read(name,&reread)&&reread==b;
}
bool Journal::Put(const char* n,const std::string& b){return Put(n,Bytes(b));}
bool Journal::Lock() {
  if(lock_>=0||!Validate())return false;
  if(!Has("lock")&&!Put("lock",std::string{}))return false;
  lock_=openat(fd_,"lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
  if(lock_<0||flock(lock_,LOCK_EX|LOCK_NB)!=0)return false;
  return Validate();
}
bool Journal::Backup(std::vector<uint8_t>* bytes) const {return Read("korea_sel_db.before",bytes)&&Classify(*bytes)==DbState::kStock;}
bool Journal::SaveBefore(const std::vector<uint8_t>& b) {
  if(!Validate()||Classify(b)!=DbState::kStock)return false;
  if(Has("korea_sel_db.before")) {
    std::vector<uint8_t> saved;
    return Read("korea_sel_db.before",&saved,true)&&Classify(saved)==DbState::kStock&&saved==b;
  }
  return Put("korea_sel_db.before",b); // New backup still uses O_EXCL + fsync/reread.
}
bool Journal::HasOrigin() const {return Has("korea_sel_db.pending")||(legacy_&&Has("legacy.origin"));}
bool Journal::ImportLegacy(const std::vector<uint8_t>& before) {
 if(!legacy_||HasOrigin()||!Validate()||!SaveBefore(before))return false;
 return Put("legacy.origin",kLegacyOrigin);
}
bool Journal::Checkpoint(bool rollback) {
  std::vector<uint8_t> b;
  if(!Validate()||!Backup(&b)||(rollback&&!HasOrigin()))return false;
  return Put(rollback?"rollback.pending":"korea_sel_db.pending",Pending(rollback));
}
bool Journal::Verify(bool rollback) {return Validate()&&Has(rollback?"rollback.pending":"korea_sel_db.pending")&&Put(rollback?"rollback.verified":"korea_sel_db.verified",Verified(rollback));}
bool Journal::EnterWorker(bool rollback) {
  std::vector<uint8_t> b;
  return Validate()&&Backup(&b)&&Has(rollback?"rollback.pending":"korea_sel_db.pending")&&Put(rollback?"rollback.worker":"apply.worker",Entered(rollback));
}
}
