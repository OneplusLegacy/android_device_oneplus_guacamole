// Root read-only legacy authority: no modem libraries, no file writes, no command forwarding.
#include "production_storage.h"
#include "w1_journal.h"
#include <cutils/sockets.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <selinux/selinux.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <cerrno>
using namespace guacamole_production;
using namespace guacamole_efs::w1;
namespace {
uint8_t AutoEvidence(){
 // 0 invalid/unreadable, 1 absent, 2 exact permanent historical evidence.
 struct stat present{};
 if(lstat("/data/vendor/guacamole_pdc_experiment",&present)<0)return errno==ENOENT?1:0;
 const char* label="u:object_r:guacamole_pdc_experiment_data_file:s0";
 int d=OpenDirectory("/data/vendor/guacamole_pdc_experiment",0,label);if(d<0)return false;
 int f=openat(d,"slot0.pending",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);int saved_errno=errno;close(d);if(f<0)return saved_errno==ENOENT?1:0;
 struct stat st{};std::vector<uint8_t>b(sizeof(kAutoEvidence)-1);uint8_t extra;
 bool ok=fstat(f,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_gid==0&&(st.st_mode&07777)==0600&&st.st_nlink==1&&st.st_size==static_cast<off_t>(b.size())&&Label(f,label)&&read(f,b.data(),b.size())==static_cast<ssize_t>(b.size())&&read(f,&extra,1)==0;
 close(f);return ok&&std::string(b.begin(),b.end())==kAutoEvidence?2:0;
}
}
int main(){
 if(geteuid()!=0||Context()!=kBrokerContext)return 4;
 int server=android_get_control_socket("guacamole_efs_evidence");if(server<0||listen(server,4))return 4;
 for(;;){
  int fd=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);if(fd<0)continue;
  timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  ucred peer{};socklen_t n=sizeof(peer);char* sid=nullptr;
  bool allowed=getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)==0&&peer.uid==kDaemonUid&&peer.gid==kDaemonUid&&getpeercon(fd,&sid)==0&&sid&&std::string(sid)==kDaemonContext;
  if(sid)freecon(sid);
  char request=0;if(!allowed||recv(fd,&request,1,MSG_TRUNC)!=1||(request!='O'&&request!='R')){close(fd);continue;}
  std::vector<uint8_t> answer(8,0);answer[0]=1;answer[1]=AutoEvidence();
  struct stat exists{};
  bool absent=lstat(kLegacyDir,&exists)<0&&errno==ENOENT;
  answer[2]=absent;
  int dir=OpenDirectory(kLegacyDir,0,kLegacyLabel);
  if(dir>=0){
   int lock=openat(dir,"lock",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
   Journal journal(dir,0,[](int f){return Label(f,kLegacyLabel);});
   answer[2]=lock>=0&&flock(lock,LOCK_SH|LOCK_NB)==0&&journal.Validate();
   if(answer[2]){
    answer[3]=journal.Has("korea_sel_db.pending");answer[4]=journal.Has("apply.worker");answer[5]=journal.Has("korea_sel_db.verified");
    std::vector<uint8_t> before;answer[6]=journal.Backup(&before);answer[7]=journal.Has("rollback.pending");
    if(request=='R'&&answer[1]==2&&answer[3]&&answer[6]&&!answer[7])answer.insert(answer.end(),before.begin(),before.end());
   }
   if(lock>=0)close(lock);
  }
  send(fd,answer.data(),answer.size(),MSG_NOSIGNAL);close(fd);
 }
}
