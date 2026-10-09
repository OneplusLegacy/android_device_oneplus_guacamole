#pragma once
#include <sys/types.h>
#include <functional>
#include "w1_experiment.h"
namespace guacamole_efs::w1 {
// Takes ownership of a directory fd. Device wrapper separately checks SELinux
// label and every fixed path component. Tests inject a temporary directory fd.
class Journal {
 public:
  explicit Journal(int fd,uid_t owner=0,std::function<bool(int)> label={},bool legacy=false):fd_(fd),owner_(owner),label_(std::move(label)),legacy_(legacy){}
  ~Journal();
  Journal(const Journal&)=delete;
  Journal& operator=(const Journal&)=delete;
  bool Validate() const;
  bool Lock();
  int lock_fd() const {return lock_;}
  bool Has(const char* name) const;
  bool Backup(std::vector<uint8_t>* bytes) const;
  bool SaveBefore(const std::vector<uint8_t>& bytes);
  bool Checkpoint(bool rollback);
  bool Verify(bool rollback);
  bool EnterWorker(bool rollback);
  bool ImportLegacy(const std::vector<uint8_t>& before);
  bool HasOrigin() const;
 private:
  bool Read(const char* name,std::vector<uint8_t>* bytes,bool durable=false) const;
  bool Put(const char* name,const std::vector<uint8_t>& bytes);
  bool Put(const char* name,const std::string& bytes);
  int fd_=-1,lock_=-1;
  uid_t owner_;
  std::function<bool(int)> label_;
  bool legacy_=false;
};
}
