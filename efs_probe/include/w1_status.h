#pragma once
#include <string>
namespace guacamole_efs::w1 {
inline constexpr char kStatusDir[]="/data/vendor/guacamole_efs_observation";
// Diagnostic cache ONLY, never an authorization/preflight input. The daemon
// cannot execute a worker or refresh it. Old boot/age/corruption => UNKNOWN.
struct Status {
 bool valid=false, profiles=false, review=true;
 std::string selected="UNKNOWN",auto0="UNKNOWN",auto1="UNKNOWN",hash;
 long age_seconds=-1;
 std::string Diagnostic() const;
};
Status ParseStatus(const std::string& text,const std::string& boot,long now);
Status ReadStatus();
bool PublishStatus(const Status& status);
}
