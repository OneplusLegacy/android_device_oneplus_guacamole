#include <gtest/gtest.h>
#include "production_core.h"
#include "carrier_catalog.h"
#include "w1_journal.h"
#include "w1_artifact.h"
#include <atomic>
#include <future>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
using namespace guacamole_production;
using namespace guacamole_efs::w1;
namespace {
const Caller controller{10123,"u:r:guacamole_efs_controller:s0"};
LegacyEvidence History(){LegacyEvidence l;l.available=l.valid=l.auto_valid=l.auto_exact=l.pending=l.worker=l.verified=l.backup=true;return l;}
Radio Ready(){Radio r;r.active=0;r.pending=-2;r.profiles.fill(1);r.slots[0]={3,1,0,0};r.slots[1]={0,0,0,0};return r;}
TEST(ProductionAuth, ExactControllerSidAndDedicatedAppUid){EXPECT_TRUE(AuthorizedController(controller));}
TEST(ProductionAuth, RootShellSystemDenied){for(unsigned uid:{0u,1000u,2000u})EXPECT_FALSE(AuthorizedController({uid,controller.sid}));}
TEST(ProductionAuth, OtherDomainsDenied){for(const char* sid:{"u:r:system_app:s0","u:r:su:s0","u:r:shell:s0","","u:r:guacamole_efs_controller:s0:spoof"})EXPECT_FALSE(AuthorizedController({10123,sid}));}
TEST(ProductionAuth, IsolatedAndOtherUserDenied){EXPECT_FALSE(AuthorizedController({99000,controller.sid}));EXPECT_FALSE(AuthorizedController({110123,controller.sid}));}
TEST(ProductionAuth, RejectionBeforeBackendEffects){std::atomic<int> effects=0;Coordinator c([&](Action){effects++;return Snapshot{};});for(unsigned uid:{0u,1000u,2000u})EXPECT_FALSE(c.Submit(Action::Install,{uid,controller.sid}));EXPECT_EQ(effects.load(),0);}
TEST(ProductionAuth, ExactCallerQueuesSingleOperation){std::promise<Action> effect;auto result=effect.get_future();Coordinator c([&](Action a){effect.set_value(a);return Snapshot{};});ASSERT_TRUE(c.Submit(Action::Install,controller));ASSERT_EQ(result.wait_for(std::chrono::seconds(2)),std::future_status::ready);EXPECT_EQ(result.get(),Action::Install);}
TEST(ProductionAuth, InvalidActionNeverQueued){std::atomic<int> effects=0;Coordinator c([&](Action){effects++;return Snapshot{};});EXPECT_FALSE(c.Submit(static_cast<Action>(42),controller));EXPECT_EQ(effects.load(),0);}
TEST(CarrierCatalog, InvalidEnumNoFallback){EXPECT_EQ(guacamole_catalog::Find(-1),nullptr);EXPECT_EQ(guacamole_catalog::Find(4),nullptr);EXPECT_TRUE(guacamole_catalog::Id(44).empty());}
TEST(CarrierCatalog, ExactKtLguMetadata){EXPECT_TRUE(guacamole_catalog::Metadata(2,39544,134293504,"Commercial-KT"));EXPECT_TRUE(guacamole_catalog::Metadata(3,41088,134293000,"Commercial-LGU"));EXPECT_FALSE(guacamole_catalog::Metadata(2,39544,123,"Commercial-KT"));EXPECT_FALSE(guacamole_catalog::Metadata(3,41088,134293000,"OBSERVED"));}
TEST(CarrierCatalog, FrozenArtifactAndMapping){EXPECT_EQ(Classify(PackagedOriginal()),DbState::kStock);EXPECT_EQ(Classify(PackagedProposal()),DbState::kKorea);EXPECT_EQ(guacamole_catalog::kProfiles[1].index,39);EXPECT_EQ(guacamole_catalog::kProfiles[2].index,40);EXPECT_EQ(guacamole_catalog::kProfiles[3].index,38);}
TEST(ProductionHistory, VerifiedHistoricalTargetInstalled){EXPECT_EQ(Recovery(DbState::kKorea,true,false,false,false,History()),"INSTALLED_VERIFIED_HISTORY");}
TEST(ProductionHistory, PendingWithoutVerifiedRequiresRecovery){auto l=History();l.verified=false;EXPECT_EQ(Recovery(DbState::kKorea,true,false,false,false,l),"RECOVERY_REQUIRED");}
TEST(ProductionHistory, UnknownNeverInstalled){EXPECT_EQ(Recovery(DbState::kUnknown,true,false,false,false,History()),"HARD_STOP");}
TEST(ProductionHistory, ProductionVerifiedTargetInstalled){EXPECT_EQ(Recovery(DbState::kKorea,true,true,true,false,History()),"INSTALLED_VERIFIED_PRODUCTION");}
TEST(ProductionPolicy, ExactTargetNoopDespiteSktAndRetainedPending){auto r=Ready();r.active=1;EXPECT_EQ(Decide(Action::Install,DbState::kKorea,r,History(),true,true,false,true),Disposition::Noop);}
TEST(ProductionPolicy, UnknownNoWrite){for(auto a:{Action::Install,Action::Rollback})EXPECT_EQ(Decide(a,DbState::kUnknown,Ready(),History(),true,true,false,true),Disposition::Block);}
TEST(ProductionPolicy, StockInstallRequiresAllPrerequisites){auto l=History();l.pending=false;EXPECT_EQ(Decide(Action::Install,DbState::kStock,Ready(),l,true,false,false,false),Disposition::Install);auto r=Ready();r.active=1;EXPECT_EQ(Decide(Action::Install,DbState::kStock,r,l,true,false,false,false),Disposition::Block);}
TEST(ProductionPolicy, PendingForeverBlocksStockReapply){EXPECT_EQ(Decide(Action::Install,DbState::kStock,Ready(),History(),true,true,false,true),Disposition::Block);}
TEST(ProductionPolicy, KnownActiveNoneRollback){for(int i=0;i<4;++i){auto r=Ready();r.active=i;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,r,History(),true,false,false,false),Disposition::Rollback);}}
TEST(ProductionPolicy, UnknownOrPendingRadioBlocksRollback){auto r=Ready();r.active=-1;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,r,History(),true,false,false,false),Disposition::Block);r=Ready();r.pending=1;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,r,History(),true,false,false,false),Disposition::Block);}
TEST(ProductionPolicy, ProfileAutoMismatchBlocksRollback){auto r=Ready();r.profiles[2]=2;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,r,History(),true,false,false,false),Disposition::Block);r=Ready();r.slots[0].refresh=-1;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,r,History(),true,false,false,false),Disposition::Block);}
TEST(ProductionPolicy, NoCapturedBackupBlocksRollback){auto l=History();l.backup=false;EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,Ready(),l,true,false,false,false),Disposition::Block);}
TEST(ProductionPolicy, PriorRollbackBlocksForever){EXPECT_EQ(Decide(Action::Rollback,DbState::kKorea,Ready(),History(),true,true,true,true),Disposition::Block);}
TEST(ProductionPolicy, ObservationNeverMutates){EXPECT_EQ(Decide(Action::Observe,DbState::kKorea,Ready(),History(),true,true,false,true),Disposition::ObserveOnly);}
class ProductionJournal:public testing::Test {
 protected:
 std::string dir;
 void SetUp() override{char path[]="/tmp/guacamole-d1-XXXXXX";char* d=mkdtemp(path);ASSERT_NE(d,nullptr);dir=d;}
 void TearDown() override{std::filesystem::remove_all(dir);}
 int Open(){return open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);}
};
TEST_F(ProductionJournal, LegacyCaptureCreatesProvenanceNotFakeApply){Journal j(Open(),getuid(),{},true);ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.ImportLegacy(PackagedOriginal()));EXPECT_TRUE(j.HasOrigin());EXPECT_TRUE(j.Has("legacy.origin"));EXPECT_FALSE(j.Has("korea_sel_db.pending"));ASSERT_TRUE(j.Checkpoint(true));ASSERT_TRUE(j.EnterWorker(true));EXPECT_FALSE(j.EnterWorker(true));EXPECT_TRUE(j.Validate());}
TEST_F(ProductionJournal, InvalidLegacyBytesNotImported){Journal j(Open(),getuid(),{},true);ASSERT_TRUE(j.Lock());EXPECT_FALSE(j.ImportLegacy(PackagedProposal()));EXPECT_FALSE(j.HasOrigin());}
TEST_F(ProductionJournal, LegacyImportRequiresExplicitAdapterOptIn){Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());EXPECT_FALSE(j.ImportLegacy(PackagedOriginal()));}
TEST_F(ProductionJournal, LegacyOriginRetainedAcrossCoordinator){ {Journal j(Open(),getuid(),{},true);ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.ImportLegacy(PackagedOriginal()));} Journal j(Open(),getuid(),{},true);ASSERT_TRUE(j.Lock());EXPECT_TRUE(j.HasOrigin());EXPECT_FALSE(j.ImportLegacy(PackagedOriginal()));}
TEST(ProductionPolicy, FreshInstallDoesNotRequireInventedHistoricalMarker){auto l=History();l.pending=l.worker=l.verified=l.backup=l.auto_exact=false;EXPECT_EQ(Decide(Action::Install,DbState::kStock,Ready(),l,true,false,false,false),Disposition::Install);}
TEST(ProductionPolicy, CorruptPresentAutoEvidenceBlocks){auto l=History();l.pending=false;l.auto_valid=false;EXPECT_EQ(Decide(Action::Install,DbState::kStock,Ready(),l,true,false,false,false),Disposition::Block);}
}

TEST(ProductionRecovery, VerifiedExplicitRollbackIsDistinctFromAutoRestoration) {
 using namespace guacamole_production; LegacyEvidence e;e.available=true;e.valid=true;
 EXPECT_EQ(Recovery(guacamole_efs::w1::DbState::kStock,true,true,true,true,e,true),"RESTORED_VERIFIED_PRODUCTION");
}
