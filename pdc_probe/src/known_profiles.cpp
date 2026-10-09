#include "known_profiles.h"
#include "carrier_catalog.h"
#include <algorithm>
namespace guacamole_pdc {
const char* CarrierToString(Carrier c){switch(c){case Carrier::kSkt:return "SKT";case Carrier::kKt:return "KT";case Carrier::kLgu:return "LGU";}return "UNKNOWN";}
namespace {
KnownProfile Profile(int i,const char* name){const auto& p=guacamole_catalog::kProfiles[i];KnownProfile r{name,p.mbn_path,p.size,{}};auto h=guacamole_catalog::Hex(p.sha256);std::copy(h.begin(),h.end(),r.expected_sha256.begin());return r;}
}
const KnownProfile& GetSktProfile(){static const auto p=Profile(1,"skt");return p;}
const KnownProfile& GetKtProfile(){static const auto p=Profile(2,"kt");return p;}
const KnownProfile& GetLguProfile(){static const auto p=Profile(3,"lgu");return p;}
const KnownProfile* GetProfileForCarrier(Carrier c){switch(c){case Carrier::kSkt:return &GetSktProfile();case Carrier::kKt:return &GetKtProfile();case Carrier::kLgu:return &GetLguProfile();}return nullptr;}
}
