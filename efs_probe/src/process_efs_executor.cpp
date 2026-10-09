#include "process_efs_executor.h"
#include "hash.h"
namespace guacamole_efs {
ProcessResult ProcessEfsExecutor::ReadTarget(EfsTarget t,const std::string& id) {
  return Run({ProcessOperation::kRead,t,id,{}});
}
ProcessResult ProcessEfsExecutor::WriteTarget(EfsWriteTarget t,const std::vector<uint8_t>& bytes,const std::string& id) {
  if(t!=EfsWriteTarget::kMcfgSelDb) return {};
  return Run({ProcessOperation::kWrite,CorrespondingReadTarget(t),id,bytes});
}
ProcessResult ProcessEfsExecutor::Run(ProcessRequest request) {
  std::lock_guard<std::mutex> guard(lock_);
  ProcessResult r;
  if(poisoned_ || !ValidProcessRequest(request)) { r.error="executor poisoned or invalid request"; return r; }
  r=launcher_.Run(request);
  if(r.spawned && !r.reaped) poisoned_=true;
  bool valid=r.spawned && r.reaped && !r.timed_out && r.signal==0 && r.exit_code==0 && r.report_valid;
  if(valid) {
    auto encoded=EncodeProcessReport(r.report);
    ProcessReport checked; std::string error;
    valid=!encoded.empty() && DecodeProcessReport(encoded,request,r.report.nonce,&checked,&error) &&
        r.report.nonce!=WorkerNonce{} && seen_.insert(r.report.nonce).second;
  }
  if(!valid) {
    r.report_valid=false;
    // No trustworthy acknowledgement after a spawn can prove no mutation.
    r.report.outcome=request.operation==ProcessOperation::kWrite && r.spawned ? ProcessOutcome::kAmbiguous : ProcessOutcome::kNotAttempted;
    if(r.report.failure!=ProcessFailure::kClosed)
      r.report.failure=r.timed_out?ProcessFailure::kTimeout:
          !r.spawned?ProcessFailure::kTransport:ProcessFailure::kProtocol;
    r.report.bytes.clear();
  }
  return r;
}
}
