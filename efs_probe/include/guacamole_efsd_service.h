#pragma once
#include <aidl/com/guacamole/efsd/BnGuacamoleEfsd.h>
#include "production_core.h"
namespace guacamole_efsd {
class GuacamoleEfsdService:public aidl::com::guacamole::efsd::BnGuacamoleEfsd {
 public:
  GuacamoleEfsdService();
  ndk::ScopedAStatus getStatus(aidl::com::guacamole::efsd::GuacamoleEfsdStatus*) override;
  ndk::ScopedAStatus prepareKoreanSupport(aidl::com::guacamole::efsd::GuacamoleEfsdResult*) override;
  ndk::ScopedAStatus applyCarrier(aidl::com::guacamole::efsd::Slot,aidl::com::guacamole::efsd::Carrier,aidl::com::guacamole::efsd::GuacamoleEfsdResult*) override;
  ndk::ScopedAStatus restoreOriginal(aidl::com::guacamole::efsd::GuacamoleEfsdResult*) override;
  ndk::ScopedAStatus getLastError(std::string*) override;
  ndk::ScopedAStatus observe(aidl::com::guacamole::efsd::ProductionStatus*) override;
  ndk::ScopedAStatus installKoreaConfiguration(aidl::com::guacamole::efsd::ProductionActionResult*) override;
  ndk::ScopedAStatus rollbackKoreaConfiguration(aidl::com::guacamole::efsd::ProductionActionResult*) override;
 private:
  ndk::ScopedAStatus Submit(guacamole_production::Action,aidl::com::guacamole::efsd::ProductionActionResult*);
  guacamole_production::Coordinator coordinator_;
};
}
