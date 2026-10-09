#pragma once
#include <array>
namespace guacamole_production {
// Carrier catalog indices; -1 UNKNOWN, -2 NONE. Presence -1 UNKNOWN,0 ABSENT,1 EXACT,2 MISMATCH.
struct Feature {int mode=-1,flex=-1,refresh=-1,carrier=-1;};
struct Radio {
 int active=-1,pending=-1;
 std::array<int,4> profiles{{-1,-1,-1,-1}};
 std::array<Feature,2> slots{};
 bool ExactProfiles() const {for(int p:profiles)if(p!=1)return false;return true;}
 bool AutoExact() const {return slots[0].mode==3&&slots[0].flex==1&&slots[0].refresh==0&&slots[1].mode==0&&slots[1].flex==0;}
 bool InstallAllowed() const {return active==0&&pending==-2&&ExactProfiles()&&AutoExact();}
 bool RollbackAllowed() const {return active>=0&&active<4&&pending==-2&&ExactProfiles()&&AutoExact();}
};
Radio ReadRadio(); // Fixed catalog IDs only. No PDC mutation operation exposed.
}
