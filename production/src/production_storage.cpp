#include "production_storage.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <selinux/selinux.h>
namespace guacamole_production {
std::string Context(){char* c=nullptr;if(getcon(&c)<0||!c)return {};std::string s=c;freecon(c);return s;}
bool Label(int fd,const char* expected){char* c=nullptr;bool ok=fgetfilecon(fd,&c)>=0&&c&&std::string(c)==expected;if(c)freecon(c);return ok;}
int OpenDirectory(const char* path,unsigned owner,const char* label){
 int fd=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;
 std::string p=path;size_t pos=1;
 while(pos<p.size()){
  size_t end=p.find('/',pos);if(end==std::string::npos)end=p.size();auto part=p.substr(pos,end-pos);
  if(part.empty()||part=="."||part==".."){close(fd);return -1;}
  int n=openat(fd,part.c_str(),(end==p.size()?O_RDONLY:O_PATH)|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=n;if(fd<0)return -1;pos=end+1;
 }
 struct stat st{};
 if(fstat(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=owner||st.st_gid!=owner||(st.st_mode&07777)!=0700||!Label(fd,label)){close(fd);return -1;}
 return fd;
}
bool ProductionFileLabel(int fd){return Label(fd,kDataLabel);}
}
