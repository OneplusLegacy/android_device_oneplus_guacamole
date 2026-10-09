#include "production_worker.h"
#include "w1_artifact.h"
#include "efs_sessions.h"
#include "efs_reader.h"
#include "efs_writer.h"
#include "hash.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <sstream>
using namespace guacamole_production;
using namespace guacamole_efs;
using namespace guacamole_efs::w1;
int main(int argc,char**) {
 if(argc!=1||geteuid()!=kDaemonUid||getegid()!=kDaemonUid||Context()!=kWorkerContext||getppid()<=1)return 4;
 struct stat a{},r{},lock{},expected{};
 if(fstat(3,&r)||!S_ISFIFO(r.st_mode)||fstat(4,&a)||!S_ISFIFO(a.st_mode)||fstat(5,&lock)||!S_ISREG(lock.st_mode)||lock.st_uid!=kDaemonUid||lock.st_gid!=kDaemonUid||(lock.st_mode&07777)!=0600||lock.st_nlink!=1||!ProductionFileLabel(5))return 4;
 char cap[160];if(fcntl(4,F_SETFL,O_NONBLOCK)<0)return 4;
 ssize_t n=read(4,cap,sizeof(cap));char extra;
 if(n<=0||n==static_cast<ssize_t>(sizeof(cap))||read(4,&extra,1)!=0)return 4;close(4);
 std::istringstream input(std::string(cap,n));std::string nonceText,op,id,other;
 if(!std::getline(input,nonceText)||!std::getline(input,op)||!std::getline(input,id)||std::getline(input,other))return 4;
 WorkerNonce nonce{};if(!ParseWorkerNonce(nonceText,&nonce)||nonce==WorkerNonce{})return 4;
 if(op!="read"&&op!="apply"&&op!="rollback")return 4;
 const bool writing=op!="read",rollback=op=="rollback";
 if(writing&&id!=op)return 4;
 int dir=OpenDirectory(kDir,kDaemonUid,kDataLabel);if(dir<0)return 4;
 if(fstatat(dir,"lock",&expected,AT_SYMLINK_NOFOLLOW)||expected.st_dev!=lock.st_dev||expected.st_ino!=lock.st_ino){close(dir);return 4;}
 if(flock(5,LOCK_EX|LOCK_NB)!=0){close(dir);return 4;}
 Journal journal(dir,kDaemonUid,ProductionFileLabel,true);if(!journal.Validate())return 4;
 ProcessRequest request;request.target=EfsTarget::kMcfgSelDb;request.transaction_id=id;request.operation=writing?ProcessOperation::kWrite:ProcessOperation::kRead;
 if(writing){
  if(rollback){if(!journal.Backup(&request.bytes))return 4;}else request.bytes=PackagedProposal();
  if(Classify(request.bytes)!=(rollback?DbState::kStock:DbState::kKorea)||!journal.EnterWorker(rollback))return 4;
 }
 if(!ValidProcessRequest(request))return 4;
 ProcessReport report;report.identity=request;report.nonce=nonce;report.failure=ProcessFailure::kTransport;
 {
  DiagEfsSessionFactory factory([]{return std::make_unique<DiagCallbackShim>();});std::string error;
  if(writing){
   auto session=factory.OpenWrite(&error);
   if(session){report.outcome=ProcessOutcome::kAmbiguous;const auto result=WriteWholeFile(&session->client(),EfsWriteTarget::kMcfgSelDb,request.bytes);
    if(result.state==EfsWriteFileState::kDone&&!result.mutation_outcome_ambiguous){report.outcome=ProcessOutcome::kConfirmed;report.failure=ProcessFailure::kNone;}}
  }else{
   auto session=factory.OpenRead(&error);
   if(session){const auto result=ReadWholeFile(&session->client(),TargetPath(EfsTarget::kMcfgSelDb));if(result.state==EfsReadFileState::kDone){report.bytes=result.data;report.outcome=ProcessOutcome::kConfirmed;report.failure=ProcessFailure::kNone;}}
  }
 }
 report.teardown_completed=true;report.sha256=Sha256(report.bytes.data(),report.bytes.size());
 const auto frame=EncodeProcessReport(report);if(frame.empty())return 4;
 size_t done=0;while(done<frame.size()){ssize_t count=write(3,frame.data()+done,frame.size()-done);if(count<0&&errno==EINTR)continue;if(count<=0)return 4;done+=count;}
 close(3);close(5);return 0;
}
