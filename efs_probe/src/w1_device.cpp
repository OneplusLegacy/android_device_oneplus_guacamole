#ifdef GUACAMOLE_W1_DEBUG
#include "w1_device.h"
#include "w1_artifact.h"
#include "efs_sessions.h"
#include "efs_reader.h"
#include "efs_writer.h"
#include "hash.h"
#include <android-base/properties.h>
#include <selinux/selinux.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
namespace guacamole_efs::w1 {
std::string SelfContext() {char* c=nullptr;if(getcon(&c)<0||!c)return {};std::string s=c;freecon(c);return s;}
bool EnvironmentAllowed() {
  const auto debug=[](const std::string& s){return s=="userdebug"||s=="eng";};
  return geteuid()==0&&getegid()==0&&SelfContext()==kContext&&
      debug(android::base::GetProperty("ro.system.build.type",""))&&
      debug(android::base::GetProperty("ro.vendor.build.type",""));
}
int OpenPrivateDirectory(const char* path,const char* context) {
  // Fixed absolute path only; reject symlinks at every component.
  int fd=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
  std::string p(path);size_t at=1;
  while(at<p.size()) {
    auto end=p.find('/',at);if(end==std::string::npos)end=p.size();
    const auto component=p.substr(at,end-at);
    if(component.empty()||component=="."||component==".."){close(fd);return -1;}
    int next=openat(fd,component.c_str(),(end==p.size()?O_RDONLY:O_PATH)|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    close(fd);fd=next;if(fd<0)return -1;at=end+1;
  }
  struct stat st{};char* label=nullptr;
  const bool good=fstat(fd,&st)==0&&S_ISDIR(st.st_mode)&&st.st_uid==0&&st.st_gid==0&&
      (st.st_mode&07777)==0700&&fgetfilecon(fd,&label)>=0&&label&&std::string(label)==context;
  if(label)freecon(label);
  if(!good){close(fd);return -1;}return fd;
}
bool AutoEvidenceVerified() {
  int d=OpenPrivateDirectory("/data/vendor/guacamole_pdc_experiment","u:object_r:guacamole_pdc_experiment_data_file:s0");
  if(d<0)return false;
  int f=openat(d,"slot0.pending",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);close(d);
  if(f<0)return false;
  struct stat st{};char* label=nullptr;
  bool ok=fstat(f,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_gid==0&&
      (st.st_mode&07777)==0600&&st.st_nlink==1&&st.st_size==sizeof(kAutoEvidence)-1&&
      fgetfilecon(f,&label)>=0&&label&&std::string(label)=="u:object_r:guacamole_pdc_experiment_data_file:s0";
  if(label)freecon(label);
  std::vector<uint8_t> data(sizeof(kAutoEvidence)-1);
  if(ok)ok=read(f,data.data(),data.size())==static_cast<ssize_t>(data.size());
  uint8_t extra;if(ok)ok=read(f,&extra,1)==0;close(f);
  return ok&&std::equal(data.begin(),data.end(),kAutoEvidence)&&Hash(data)=="e1b98ba2352d83550fcb64c5e27a808fbd6c40eb0d73dd30366b3d4c7f2e0b64";
}
bool JournalFileLabel(int fd) {
 char* c=nullptr;const bool ok=fgetfilecon(fd,&c)>=0&&c&&std::string(c)==kJournalContext;
 if(c)freecon(c);return ok;
}
int WorkerMain(int argc,char** argv) {
  // Only coordinator spawn+exec provides these private descriptors. Public init
  // services have fixed argv; no shell/su transition into this domain exists.
  if(argc!=5||std::string(argv[1])!=kWorkerCommand||!EnvironmentAllowed()||getppid()<=1)return 4;
  WorkerNonce nonce{};
  if(!ParseWorkerNonce(argv[2],&nonce))return 4;
  struct stat auth{},result{},lock{},expected{};
  if(fstat(4,&auth)!=0||!S_ISFIFO(auth.st_mode)||fstat(3,&result)!=0||!S_ISFIFO(result.st_mode)||
      fstat(5,&lock)!=0||!S_ISREG(lock.st_mode))return 4;
  WorkerNonce supplied{};
  // Parent writes 16 bytes and closes BEFORE spawning; this read cannot wait on
  // an untrusted live producer. Mark nonblocking anyway, reject any short read.
  if(fcntl(4,F_SETFL,O_NONBLOCK)<0||read(4,supplied.data(),supplied.size())!=static_cast<ssize_t>(supplied.size())||supplied!=nonce)return 4;
  uint8_t extra;if(read(4,&extra,1)!=0)return 4;close(4);
  const std::string op=argv[3],id=argv[4];
  if(op!="read"&&op!="apply"&&op!="rollback")return 4;
  const bool write=op!="read",rollback=op=="rollback";
  if(write&&id!=op)return 4;
  int directory=OpenPrivateDirectory(kJournalDir,kJournalContext);if(directory<0)return 4;
  if(fstatat(directory,"lock",&expected,AT_SYMLINK_NOFOLLOW)!=0||lock.st_dev!=expected.st_dev||lock.st_ino!=expected.st_ino){close(directory);return 4;}
  Journal journal(directory,0,JournalFileLabel);
  if(!journal.Validate()||!AutoEvidenceVerified())return 4;
  ProcessRequest request;request.target=EfsTarget::kMcfgSelDb;request.transaction_id=id;
  request.operation=write?ProcessOperation::kWrite:ProcessOperation::kRead;
  if(write) {
    if(rollback) {if(!journal.Backup(&request.bytes))return 4;}
    else request.bytes=PackagedProposal();
    if(Classify(request.bytes)!=(rollback?DbState::kStock:DbState::kKorea)||!journal.EnterWorker(rollback))return 4;
  }
  if(!ValidProcessRequest(request))return 4;
  ProcessReport report;report.identity=request;report.nonce=nonce;
  report.failure=ProcessFailure::kTransport;
  // One factory, one Init, one operation, one DeInit per OS process. No retry.
  {
    DiagEfsSessionFactory factory([]{return std::make_unique<DiagCallbackShim>();});
    std::string error;
    if(write) {
      auto session=factory.OpenWrite(&error);
      if(session) {
        report.outcome=ProcessOutcome::kAmbiguous;
        const auto r=WriteWholeFile(&session->client(),EfsWriteTarget::kMcfgSelDb,request.bytes);
        if(r.state==EfsWriteFileState::kDone&&!r.mutation_outcome_ambiguous) {
          report.outcome=ProcessOutcome::kConfirmed;report.failure=ProcessFailure::kNone;
        }
      }
    } else {
      auto session=factory.OpenRead(&error);
      if(session) {
        const auto r=ReadWholeFile(&session->client(),TargetPath(EfsTarget::kMcfgSelDb));
        if(r.state==EfsReadFileState::kDone) {
          report.outcome=ProcessOutcome::kConfirmed;report.failure=ProcessFailure::kNone;report.bytes=r.data;
        }
      }
    }
  }
  report.teardown_completed=true;report.sha256=Sha256(report.bytes.data(),report.bytes.size());
  const auto frame=EncodeProcessReport(report);if(frame.empty())return 4;
  size_t pos=0;
  while(pos<frame.size()) {ssize_t n=::write(3,frame.data()+pos,frame.size()-pos);if(n<0&&errno==EINTR)continue;if(n<=0)return 4;pos+=n;}
  close(3);close(5);return 0;
}
}
#endif
