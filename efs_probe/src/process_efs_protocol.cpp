#include "process_efs_executor.h"
#include "hash.h"
#include <algorithm>
namespace guacamole_efs {
namespace {
void Put(std::vector<uint8_t>& b, uint32_t n) { for(int i=0;i<4;++i) b.push_back(n>>(i*8)); }
uint32_t Get(const std::vector<uint8_t>& b,size_t p) { uint32_t n=0; for(int i=0;i<4;++i) n|=uint32_t(b[p+i])<<(i*8); return n; }
bool SafeTarget(EfsTarget t) {
  switch(t) {
    case EfsTarget::kMcfgSelDb: case EfsTarget::kMcfgRfsSwDigestVersion:
    case EfsTarget::kMcfgAutoselectByUim: case EfsTarget::kMcfgAutoselectByUimSubscription01:
    case EfsTarget::kMcfgSetting: case EfsTarget::kMcfgSetting1: return true;
    default: return false;
  }
}
}
bool ValidProcessRequest(const ProcessRequest& r) {
  if (!SafeTarget(r.target) || r.transaction_id.empty() || r.transaction_id.size()>128) return false;
  for(char c:r.transaction_id) if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return false;
  if(r.operation==ProcessOperation::kRead) return r.bytes.empty();
  return r.operation==ProcessOperation::kWrite && r.target==EfsTarget::kMcfgSelDb && !r.bytes.empty() && r.bytes.size()<=kProcessDataCap;
}
std::vector<uint8_t> EncodeProcessReport(const ProcessReport& r) {
  if(!ValidProcessRequest(r.identity)||r.bytes.size()>kProcessDataCap ||
      uint32_t(r.outcome)>2 || uint32_t(r.failure)>4) return {};
  if(r.outcome==ProcessOutcome::kConfirmed && (!r.teardown_completed || r.failure!=ProcessFailure::kNone)) return {};
  if(r.outcome!=ProcessOutcome::kConfirmed && !r.bytes.empty()) return {};
  if(r.sha256!=Sha256(r.bytes.data(),r.bytes.size())) return {};
  std::vector<uint8_t> b{'G','E','F','S'};
  Put(b,2); Put(b,uint32_t(r.identity.operation)); Put(b,uint32_t(r.identity.target));
  Put(b,uint32_t(r.outcome)); Put(b,uint32_t(r.failure)); Put(b,r.teardown_completed?1:0);
  Put(b,r.identity.transaction_id.size()); Put(b,r.bytes.size());
  b.insert(b.end(),r.nonce.begin(),r.nonce.end());
  b.insert(b.end(),r.sha256.begin(),r.sha256.end());
  Put(b,r.identity.bytes.size());
  auto request_hash=Sha256(r.identity.bytes.data(),r.identity.bytes.size());
  b.insert(b.end(),request_hash.begin(),request_hash.end());
  b.insert(b.end(),r.identity.transaction_id.begin(),r.identity.transaction_id.end());
  b.insert(b.end(),r.bytes.begin(),r.bytes.end());
  auto checksum=Sha256(b.data(),b.size()); b.insert(b.end(),checksum.begin(),checksum.end()); return b;
}
bool DecodeProcessReport(const std::vector<uint8_t>& b,const ProcessRequest& request,
    const WorkerNonce& nonce,ProcessReport* out,std::string* error) {
  auto fail=[&](){ if(error)*error="invalid process report identity/checksum/length/outcome"; return false; };
  if(!out||!ValidProcessRequest(request)||b.size()<152||b.size()>kProcessDataCap+280) return fail();
  if(!std::equal(b.begin(),b.begin()+4,"GEFS")||Get(b,4)!=2||Get(b,8)!=uint32_t(request.operation)||Get(b,12)!=uint32_t(request.target)) return fail();
  const uint32_t idsize=Get(b,28), size=Get(b,32);
  if(idsize!=request.transaction_id.size()||size>kProcessDataCap||b.size()!=152+idsize+size||Get(b,16)>2||Get(b,20)>4||Get(b,24)>1) return fail();
  if(!std::equal(nonce.begin(),nonce.end(),b.begin()+36)||!std::equal(request.transaction_id.begin(),request.transaction_id.end(),b.begin()+120)) return fail();
  const auto request_hash=Sha256(request.bytes.data(),request.bytes.size());
  if(Get(b,84)!=request.bytes.size() || !std::equal(request_hash.begin(),request_hash.end(),b.begin()+88)) return fail();
  auto sum=Sha256(b.data(),b.size()-32); if(!std::equal(sum.begin(),sum.end(),b.end()-32)) return fail();
  ProcessReport r; r.identity=request; r.nonce=nonce;
  r.outcome=ProcessOutcome(Get(b,16)); r.failure=ProcessFailure(Get(b,20)); r.teardown_completed=Get(b,24)==1;
  r.bytes.assign(b.begin()+120+idsize,b.end()-32); r.sha256=Sha256(r.bytes.data(),r.bytes.size());
  if(!std::equal(r.sha256.begin(),r.sha256.end(),b.begin()+52)||EncodeProcessReport(r)!=b) return fail();
  *out=std::move(r); return true;
}
}
