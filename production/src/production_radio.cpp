#include "production_radio.h"
#include "feature_readonly.h"
#include "carrier_catalog.h"
namespace guacamole_pdc { namespace {
std::vector<uint8_t> Id(size_t i){return guacamole_catalog::Id(static_cast<int>(i));}
class Guard final:public IQcciShim {
 public:
 bool Load(std::string* e) override{return inner_.Load(e);}
 QmiServiceObject PdcGetServiceObject(int32_t a,int32_t b,int32_t c) override{return inner_.PdcGetServiceObject(a,b,c);}
 int ClientInitInstance(QmiServiceObject o,QmiIndicationCallback cb,void* d,uint32_t t,QmiClientHandle* h) override{return inner_.ClientInitInstance(o,cb,d,t,h);}
 int ClientRelease(QmiClientHandle h) override{return inner_.ClientRelease(h);}
 int SendRawMsgSync(QmiClientHandle h,unsigned int m,const void* p,unsigned int n,void* out,unsigned int cap,unsigned int* got,unsigned int timeout) override {
   if(!p||!n||n>256)return -1;
   const auto* raw=static_cast<const uint8_t*>(p);const auto token=PeekIndicationToken(raw,n);
   if(!token)return -1;
   std::vector<uint8_t> b(raw,raw+n);bool allowed=false;
   if(m==kMsgGetSelectedConfig)allowed=b==BuildGetSelectedConfigRequest(*token,ConfigType::kSoftware);
   if(m==kMsgGetConfigInfo)for(size_t i=0;i<4;++i)allowed=allowed||b==BuildGetConfigInfoRequest(*token,ConfigType::kSoftware,Id(i));
   if(!allowed)return -1;
   return inner_.SendRawMsgSync(h,m,p,n,out,cap,got,timeout);
 }
 private:QcciShim inner_;
};
}
}
namespace guacamole_production {
Radio ReadRadio() {
 using namespace guacamole_pdc;
 Radio r;
 {
  Guard guard; PdcClient client(guard);std::string log;
  if(client.Init({{1,15,6}},&log)) {
   const auto s=client.GetSelectedConfig(ConfigType::kSoftware,5000);
   if(s.status==ProbeStatus::kOk&&s.data.ok&&!s.data.error_status&&!s.data.error_code){
    r.active=s.data.active_id?guacamole_catalog::Identify(*s.data.active_id):-2;
    r.pending=s.data.pending_id?guacamole_catalog::Identify(*s.data.pending_id):-2;
   }
   for(int i=0;i<4;++i){
    const auto o=client.GetConfigInfo(ConfigType::kSoftware,guacamole_catalog::Id(i),5000);
    const auto& d=o.data;
    if(o.status==ProbeStatus::kOk&&d.ok&&!d.error_status&&!d.error_code)
     r.profiles[i]=d.has_total_size&&d.has_version&&d.has_description&&guacamole_catalog::Metadata(i,d.total_size,d.version,d.description)?1:2;
    else if(o.status==ProbeStatus::kIndicationRejected&&d.ok&&d.error_status==50&&!d.error_code)r.profiles[i]=0;
   }
  }
 }
 for(int i=0;i<2;++i){
  QcciShim shim;const auto slot=i==0?FeatureSlot::kSlot0:FeatureSlot::kSlot1;
  FeatureReadQcciGuard guard(shim,slot);FeatureReadClient client(guard);
  const auto o=client.Read(slot,5000);const auto& d=o.data;
  if(o.status!=ProbeStatus::kOk||o.sync_result||o.sync_error||!d.valid||d.error)continue;
  if(d.selection_mode)r.slots[i].mode=*d.selection_mode;
  if(d.flex_mapping)r.slots[i].flex=*d.flex_mapping?1:0;
  if(d.refresh_mode)r.slots[i].refresh=*d.refresh_mode;
  if(d.selection_carrier)r.slots[i].carrier=*d.selection_carrier;
 }
 return r;
}
}
