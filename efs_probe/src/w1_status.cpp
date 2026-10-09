#include "w1_status.h"
#include "w1_experiment.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <map>
namespace guacamole_efs::w1 {
namespace {
constexpr long kLifetime=300;
std::string BootId() {
 int f=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return {};
 char b[64];ssize_t n=read(f,b,sizeof(b));close(f);
 if(n!=37||b[36]!='\n')return {};
 std::string s(b,36);for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||c=='-'))return {};
 return s;
}
long Now(){timespec t{};return clock_gettime(CLOCK_BOOTTIME,&t)==0?t.tv_sec:-1;}
std::string Digest(const std::string& s){return Hash({s.begin(),s.end()});}
bool WriteAll(int f,const std::string& s){size_t p=0;while(p<s.size()){auto n=write(f,s.data()+p,s.size()-p);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;}return true;}
int Directory(){int f=open(kStatusDir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat st{};
 if(f>=0&&(fstat(f,&st)!=0||st.st_uid!=0||st.st_gid!=2909||(st.st_mode&07777)!=02750)){close(f);f=-1;}return f;}
}
std::string Status::Diagnostic() const {
 if(!valid)return "W1_OBSERVATION_V1\nvalid=false\nreview=true\nproduction=BLOCKED_MAPPING_VALIDATION\n";
 return "W1_OBSERVATION_V1\nvalid=true\nselected="+selected+"\nauto0="+auto0+"\nauto1="+auto1+
     "\nhash="+hash+"\nprofiles="+(profiles?"true":"false")+"\nreview="+(review?"true":"false")+
     "\nage_seconds="+std::to_string(age_seconds)+"\nproduction=BLOCKED_MAPPING_VALIDATION\n";
}
bool PublishStatus(const Status& s) {
 const auto boot=BootId();const auto now=Now();if(boot.empty()||now<0)return false;
 // Restricted ASCII vocabulary, no XML/SIM identifiers or paths in this cache.
 const auto safe=[](const std::string& v){return v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/,=-")==std::string::npos;};
 if(!safe(s.selected)||!safe(s.auto0)||!safe(s.auto1)||!safe(s.hash))return false;
 std::string body="version=1\nboot="+boot+"\ntime="+std::to_string(now)+"\nselected="+s.selected+
   "\nauto0="+s.auto0+"\nauto1="+s.auto1+"\nhash="+s.hash+"\nprofiles="+(s.profiles?"1":"0")+"\nreview="+(s.review?"1":"0")+"\n";
 const std::string content=body+"checksum="+Digest(body)+"\n";
 int d=Directory();if(d<0)return false;
 const std::string temp="observation-"+std::to_string(getpid())+"-"+std::to_string(now);
 int f=openat(d,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0640);
 if(f<0){close(d);return false;}
 // Directory setgid supplies the daemon group; no CAP_CHOWN is needed.
 struct stat st{};bool ok=fchmod(f,0640)==0&&fstat(f,&st)==0&&st.st_uid==0&&st.st_gid==2909&&(st.st_mode&07777)==0640;
 if(ok)ok=WriteAll(f,content)&&fsync(f)==0;
 if(close(f)!=0)ok=false;
 if(ok)ok=renameat(d,temp.c_str(),d,"latest")==0&&fsync(d)==0;
 close(d);return ok;
}
Status ReadStatus() {
 Status r;int d=Directory();if(d<0)return r;
 int f=openat(d,"latest",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);close(d);if(f<0)return r;
 struct stat st{};bool ok=fstat(f,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_gid==2909&&
     (st.st_mode&07777)==0640&&st.st_nlink==1&&st.st_size>0&&st.st_size<2048;
 std::string text;
 if(ok){text.resize(st.st_size);size_t p=0;while(p<text.size()){auto n=read(f,text.data()+p,text.size()-p);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}p+=n;}}
 char c;if(ok&&read(f,&c,1)!=0)ok=false;close(f);if(!ok)return r;
 return ParseStatus(text,BootId(),Now());
}
Status ParseStatus(const std::string& text,const std::string& boot,long now) {
 Status r;if(boot.empty()||now<0||text.size()>2048)return r;
 auto at=text.rfind("checksum=");if(at==std::string::npos||text.substr(at)!="checksum="+Digest(text.substr(0,at))+"\n")return r;
 std::map<std::string,std::string> fields;std::istringstream input(text.substr(0,at));std::string line;
 while(std::getline(input,line)){auto split=line.find('=');if(split==std::string::npos||!fields.emplace(line.substr(0,split),line.substr(split+1)).second)return r;}
 if(fields.size()!=9||fields["version"]!="1"||fields["boot"]!=boot||fields["time"].empty())return r;
 long timestamp=0;for(char n:fields["time"]){if(n<'0'||n>'9'||timestamp>100000000)return r;timestamp=timestamp*10+n-'0';}
 auto age=now-timestamp;if(age<0||age>kLifetime)return r;
 if(fields["profiles"]!="0"&&fields["profiles"]!="1")return r;
 if(fields["review"]!="0"&&fields["review"]!="1")return r;
 if(fields["selected"]!="UNKNOWN") {
   const auto slash=fields["selected"].find('/');if(slash==std::string::npos)return r;
   const auto known=[](const std::string& s){return s=="Common"||s=="SKT"||s=="KT"||s=="LGU"||s=="NONE"||s=="UNKNOWN";};
   if(!known(fields["selected"].substr(0,slash))||!known(fields["selected"].substr(slash+1)))return r;
 }
 if(fields["hash"]!=""&&fields["hash"]!=kSourceHash&&fields["hash"]!=kTargetHash)return r;
 for(const auto* key:{"auto0","auto1"})if(fields[key]!="UNKNOWN"&&fields[key]!="mode=3,flex=true"&&fields[key]!="mode=0,flex=false")return r;
 r.valid=true;r.profiles=fields["profiles"]=="1";r.review=fields["review"]=="1";r.selected=fields["selected"];
 r.auto0=fields["auto0"];r.auto1=fields["auto1"];r.hash=fields["hash"];r.age_seconds=age;return r;
}
}
