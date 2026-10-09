#include "guacamole_efsd_service.h"
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_platform.h>
namespace guacamole_efsd {
using namespace aidl::com::guacamole::efsd;
using namespace guacamole_production;
GuacamoleEfsdService::GuacamoleEfsdService():coordinator_(Execute){}
ndk::ScopedAStatus GuacamoleEfsdService::getStatus(GuacamoleEfsdStatus* o){
 const auto s=coordinator_.Observe();o->state=GuacamoleEfsdState::MAPPING_BLOCKED;
 o->pdcSktConfigPresent=s.radio.profiles[1]==1;o->pdcKtConfigPresent=s.radio.profiles[2]==1;o->pdcLguConfigPresent=s.radio.profiles[3]==1;
 o->selDbPatched=s.xml=="KOREA_EXACT";o->preimageSha256Hex="";o->currentSha256Hex="";
 // V1 defines AUTO for indeterminate per-SIM selection; not a claim about modem or GetFeature.
 o->selectedCarrierSim1=Carrier::AUTO;o->selectedCarrierSim2=Carrier::AUTO;o->lastTransactionId="";
 o->lastResult=GuacamoleEfsdResult::BLOCKED_MAPPING;o->rebootRequired=false;return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus GuacamoleEfsdService::prepareKoreanSupport(GuacamoleEfsdResult* o){*o=GuacamoleEfsdResult::BLOCKED_MAPPING;return ndk::ScopedAStatus::ok();}
ndk::ScopedAStatus GuacamoleEfsdService::applyCarrier(Slot,Carrier,GuacamoleEfsdResult* o){*o=GuacamoleEfsdResult::BLOCKED_MAPPING;return ndk::ScopedAStatus::ok();}
ndk::ScopedAStatus GuacamoleEfsdService::restoreOriginal(GuacamoleEfsdResult* o){*o=GuacamoleEfsdResult::BLOCKED_MAPPING;return ndk::ScopedAStatus::ok();}
ndk::ScopedAStatus GuacamoleEfsdService::getLastError(std::string* o){*o=coordinator_.Observe().result;return ndk::ScopedAStatus::ok();}
ndk::ScopedAStatus GuacamoleEfsdService::observe(ProductionStatus* o){
 const auto s=coordinator_.Observe();o->modemActive=s.radio.active;o->modemPending=s.radio.pending;
 o->profilePresence.assign(s.radio.profiles.begin(),s.radio.profiles.end());
 o->slot0Mode=s.radio.slots[0].mode;o->slot0Flex=s.radio.slots[0].flex;o->slot0Refresh=s.radio.slots[0].refresh;
 o->slot1Mode=s.radio.slots[1].mode;o->slot1Flex=s.radio.slots[1].flex;o->slot1Refresh=s.radio.slots[1].refresh;
 o->selDb=s.xml;o->recovery=s.recovery;o->lastAction=s.last_action;o->lastResult=s.result;
 o->generation=s.generation;o->observedElapsedMs=s.elapsed_ms;o->ageMs=s.elapsed_ms?ElapsedMs()-s.elapsed_ms:-1;
 o->busy=s.busy;o->installCandidate=s.install_candidate&&!s.busy;o->rollbackCandidate=s.rollback_candidate&&!s.busy;return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus GuacamoleEfsdService::Submit(Action action,ProductionActionResult* out){
 const char* sid=AIBinder_getCallingSid();const Caller caller{AIBinder_getCallingUid(),sid?sid:""};
 // No observation, storage, queue, PDC or identity clearing before this gate.
 if(!AuthorizedController(caller)){*out=ProductionActionResult::DENIED;return ndk::ScopedAStatus::ok();}
 *out=coordinator_.Submit(action,caller)?ProductionActionResult::QUEUED:ProductionActionResult::BUSY;
 return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus GuacamoleEfsdService::installKoreaConfiguration(ProductionActionResult* o){return Submit(Action::Install,o);}
ndk::ScopedAStatus GuacamoleEfsdService::rollbackKoreaConfiguration(ProductionActionResult* o){return Submit(Action::Rollback,o);}
}
