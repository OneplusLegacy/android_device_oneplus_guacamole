#include "legacy_evidence.h"
#include "production_storage.h"
#include "w1_experiment.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <selinux/selinux.h>
#include <unistd.h>
#include <cstring>
namespace guacamole_production {
LegacyEvidence RequestLegacyEvidence(bool recovery){
 LegacyEvidence r;int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0)return r;
 timeval limit{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit));
 sockaddr_un a{};a.sun_family=AF_UNIX;strncpy(a.sun_path,kSocket,sizeof(a.sun_path)-1);
 if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))){close(fd);return r;}
 ucred peer{};socklen_t n=sizeof(peer);char* sid=nullptr;
 bool ok=getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)==0&&peer.uid==0&&getpeercon(fd,&sid)==0&&sid&&std::string(sid)==kBrokerContext;
 if(sid)freecon(sid);
 const char request=recovery?'R':'O';
 if(!ok||send(fd,&request,1,MSG_NOSIGNAL)!=1){close(fd);return r;}
 std::vector<uint8_t> data(guacamole_efs::w1::kSourceSize+9);
 ssize_t count=recv(fd,data.data(),data.size(),MSG_TRUNC);close(fd);
 if(count<8||static_cast<size_t>(count)>data.size()||data[0]!=1)return r;
 if(data[1]>2)return r;
 for(int i=2;i<8;++i)if(data[i]>1)return r;
 r.available=true;r.auto_valid=data[1]>0;r.auto_exact=data[1]==2;r.valid=data[2];r.pending=data[3];r.worker=data[4];r.verified=data[5];r.backup=data[6];r.rollback=data[7];
 if(count>8){r.before.assign(data.begin()+8,data.begin()+count);if(!recovery||guacamole_efs::w1::Classify(r.before)!=guacamole_efs::w1::DbState::kStock)return {};}
 return r;
}
}
