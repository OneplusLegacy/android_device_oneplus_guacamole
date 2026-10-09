#pragma once
#include "w1_journal.h"
#include "production_storage.h"
namespace guacamole_production {
inline constexpr const char* kWorkerExecutable="/vendor/bin/guacamole_efs_worker";
bool CoordinatorIdentity();
class ProductionLauncher final:public guacamole_efs::IProcessWorkerLauncher {
 public:
  explicit ProductionLauncher(guacamole_efs::w1::Journal& journal):journal_(journal){}
  guacamole_efs::ProcessResult Run(const guacamole_efs::ProcessRequest&) override;
 private: guacamole_efs::w1::Journal& journal_;
};
}
