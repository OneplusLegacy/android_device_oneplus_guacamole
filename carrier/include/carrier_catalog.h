#pragma once
#include "carrier_catalog_generated.h"
#include <string>
#include <vector>
namespace guacamole_catalog {
inline const Profile* Find(int carrier) {
 return carrier>=0&&carrier<4?&kProfiles[carrier]:nullptr;
}
inline std::vector<uint8_t> Hex(const char* s) {
 std::vector<uint8_t> b; if(!s)return b;
 const std::string text=s;
 auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
 if(text.size()%2)return {};
 for(size_t i=0;i<text.size();i+=2){int a=nibble(text[i]),z=nibble(text[i+1]);if(a<0||z<0)return {};b.push_back((a<<4)|z);}return b;
}
inline std::vector<uint8_t> Id(int carrier){const auto* p=Find(carrier);return p?Hex(p->id):std::vector<uint8_t>{};}
inline int Identify(const std::vector<uint8_t>& id){for(int i=0;i<4;++i)if(id==Id(i))return i;return -1;}
inline bool Metadata(int carrier,uint32_t size,uint32_t version,const std::string& description) {
 const auto* p=Find(carrier);return p&&size==p->size&&version==p->version&&description==p->description;
}
}
