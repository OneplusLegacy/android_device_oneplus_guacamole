#include "w1_pdc_readonly.h"
#include <array>
#include "carrier_catalog.h"
namespace guacamole_pdc {
namespace {
const std::array<const char*,4> ids={guacamole_catalog::kProfiles[0].id,guacamole_catalog::kProfiles[1].id,guacamole_catalog::kProfiles[2].id,guacamole_catalog::kProfiles[3].id};
std::vector<uint8_t> Id(size_t i){return guacamole_catalog::Id(static_cast<int>(i));}
std::string SelectionName(const std::optional<std::vector<uint8_t>>& id) {
 if(!id)return "NONE";
 const char* names[]={"Common","SKT","KT","LGU"};
 for(size_t i=0;i<4;++i)if(*id==Id(i))return names[i];
 return "UNKNOWN";
}
// Exact read request firewall, including token and config type. PdcClient's
// mutation methods exist in its library but cannot cross this shim.
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
bool Good(const FeatureReadOutcome& f) {return f.status==ProbeStatus::kOk&&f.sync_result==0&&f.sync_error==0&&f.data.valid&&f.data.error==0;}
std::string FeatureText(const FeatureReadOutcome& f) {
 if(!Good(f)||!f.data.selection_mode||!f.data.flex_mapping)return "UNKNOWN";
 return "mode="+std::to_string(*f.data.selection_mode)+",flex="+(*f.data.flex_mapping?"true":"false");
}
}
W1RadioObservation ObserveW1Radio() {
 W1RadioObservation r;
 {
  Guard guard;PdcClient client(guard);std::string log;
  if(!client.Init({{1,15,6}},&log))return r;
  const auto s=client.GetSelectedConfig(ConfigType::kSoftware,5000);
  if(s.status!=ProbeStatus::kOk||!s.data.ok)return r;
  // Only a valid successful exact GetSelected response may establish these.
  // Empty/unknown active IDs and ANY present pending TLV fail recovery closed.
  r.pending_none=!s.data.pending_id;
  if(s.data.active_id)for(size_t i=0;i<4;++i)r.active_known=r.active_known||*s.data.active_id==Id(i);
  r.common_idle=s.data.active_id==std::optional<std::vector<uint8_t>>(Id(0))&&r.pending_none;
  r.selected=SelectionName(s.data.active_id)+"/"+SelectionName(s.data.pending_id);
  const auto& profiles=guacamole_catalog::kProfiles;
  const uint32_t sizes[]={profiles[0].size,profiles[1].size,profiles[2].size,profiles[3].size};
  const uint32_t versions[]={profiles[0].version,profiles[1].version,profiles[2].version,profiles[3].version};
  const char* descriptions[]={profiles[0].description,profiles[1].description,profiles[2].description,profiles[3].description};
  r.profiles=true;
  for(size_t i=0;i<4;++i) {
   const auto x=client.GetConfigInfo(ConfigType::kSoftware,Id(i),5000);
   const auto& d=x.data;
   r.detail+="profile="+std::to_string(i)+" expected_id="+ids[i]+" status="+ProbeStatusToString(x.status)+
       " size="+std::to_string(d.total_size)+" version="+std::to_string(d.version)+
       " metadata_exact="+((x.status==ProbeStatus::kOk&&d.ok&&d.has_total_size&&d.has_description&&d.has_version&&
        d.total_size==sizes[i]&&d.version==versions[i]&&d.description==descriptions[i])?"true":"false")+"\n";
   // Do not echo arbitrary modem description strings into the structured report.

   if(x.status!=ProbeStatus::kOk||!d.ok||!d.has_total_size||!d.has_description||!d.has_version||d.total_size!=sizes[i]||d.version!=versions[i]||d.description!=descriptions[i]) {r.profiles=false;return r;}
  }
 }
 {
  QcciShim shim;FeatureReadQcciGuard guard(shim,FeatureSlot::kSlot0);FeatureReadClient client(guard);
  const auto x=client.Read(FeatureSlot::kSlot0,5000);r.auto0=FeatureText(x);
  r.slot0=Good(x)&&x.data.selection_mode==3&&x.data.flex_mapping==true;
 }
 {
  QcciShim shim;FeatureReadQcciGuard guard(shim,FeatureSlot::kSlot1);FeatureReadClient client(guard);
  const auto x=client.Read(FeatureSlot::kSlot1,5000);r.auto1=FeatureText(x);
  r.slot1=Good(x)&&x.data.selection_mode==0&&x.data.flex_mapping==false;
 }
 return r;
}
}
