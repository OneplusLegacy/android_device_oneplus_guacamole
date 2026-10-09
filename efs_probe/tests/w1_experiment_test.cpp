#include <gtest/gtest.h>
#include "w1_experiment.h"
#include "w1_artifact.h"
#include "w1_journal.h"
#include "w1_status.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
using namespace guacamole_efs;
using namespace guacamole_efs::w1;
namespace {
struct Fake:IO {
 Preconditions pre{true,true,true,true,true,true,true,true,true,true,true,true};
 bool pending=false,rollback_pending=false,backup=true,checkpoint_ok=true,save_ok=true,verify_ok=true;
 int reads=0,writes=0;std::vector<std::string> events;
 std::vector<Observation> observations{{true,PackagedOriginal(),{}},{true,PackagedProposal(),{}}};
 ProcessResult wr;
 Fake(){wr.spawned=true;wr.reaped=true;wr.exit_code=0;}
 Preconditions Preflight() override{events.push_back("preflight");return pre;}
 bool HasApplyPending() override{return pending;}
 bool HasRollbackPending() override{return rollback_pending;}
 Observation ReadFresh() override{events.push_back("spawn-read-"+std::to_string(reads));auto o=observations.at(reads++);events.push_back("reap-read");return o;}
 bool SaveBefore(const std::vector<uint8_t>& b) override {events.push_back("backup-fsync-reread");return save_ok&&b==PackagedOriginal();}
 bool BackupValid() override{return backup;}
 bool Checkpoint(bool rb) override{events.push_back("pending-fsync");if(checkpoint_ok){if(rb)rollback_pending=true;else pending=true;}return checkpoint_ok;}
 ProcessResult WriteFresh(bool rb) override{events.push_back(rb?"spawn-rollback":"spawn-write");++writes;if(wr.reaped)events.push_back("reap-write");return wr;}
 bool RecordVerified(bool) override{events.push_back("verified");return verify_ok;}
};
TEST(W1Artifact, ExactSourceAndTarget) {EXPECT_EQ(Classify(PackagedOriginal()),DbState::kStock);EXPECT_EQ(Classify(PackagedProposal()),DbState::kKorea);EXPECT_EQ(PackagedProposal().size(),58941u);}
TEST(W1Artifact, SourceChanged) {auto b=PackagedOriginal();b[0]^=1;EXPECT_EQ(Classify(b),DbState::kUnknown);}
TEST(W1Artifact, TargetChanged) {auto b=PackagedProposal();b.push_back(0);EXPECT_EQ(Classify(b),DbState::kUnknown);}
TEST(W1Gate, WrongConfirmationZeroCalls) {Fake f;auto c=ParseCommand({"apply-korea-sel-db-once","--confirm","wrong"});RunExperiment(c,f);EXPECT_TRUE(f.events.empty());}
TEST(W1Gate, ArbitraryPathAndPayloadImpossible) {EXPECT_EQ(ParseCommand({"apply-korea-sel-db-once","--confirm",kApplyConfirmation,"/tmp/x"}),Command::kInvalid);EXPECT_EQ(ParseCommand({"write","sel-db","00"}),Command::kInvalid);}
TEST(W1Gate, ExactCommandsOnly) {EXPECT_EQ(ParseCommand({"apply-korea-sel-db-once","--confirm",kApplyConfirmation}),Command::kApply);EXPECT_EQ(ParseCommand({"rollback-korea-sel-db-once","--confirm",kRollbackConfirmation}),Command::kRollback);}
TEST(W1Gate, EveryPreconditionRequired) {for(int i=0;i<10;++i){Fake f;bool* flags[]={&f.pre.root,&f.pre.debug,&f.pre.context,&f.pre.auto_evidence,&f.pre.slot0,&f.pre.slot1,&f.pre.common_idle,&f.pre.profiles,&f.pre.artifact,&f.pre.journal};*flags[i]=false;RunExperiment(Command::kApply,f);EXPECT_EQ(f.reads,0);EXPECT_EQ(f.writes,0);}}
TEST(W1Gate, AlreadyTargetNeverWritten) {Fake f;f.observations[0].bytes=PackagedProposal();auto r=RunExperiment(Command::kApply,f);EXPECT_EQ(r.outcome,"ALREADY_TARGET_REVIEW");EXPECT_EQ(f.writes,0);EXPECT_FALSE(f.pending);}
TEST(W1Gate, UnknownBaselineBlocked) {Fake f;f.observations[0].bytes={1};EXPECT_EQ(RunExperiment(Command::kApply,f).outcome,"UNKNOWN_BASELINE");EXPECT_EQ(f.writes,0);}
TEST(W1Gate, InspectNeverWrites) {Fake f;f.observations[1].bytes=PackagedOriginal();RunExperiment(Command::kInspect,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,2);EXPECT_FALSE(f.pending);}
TEST(W1Gate, InspectCanObserveChangedPdc) {Fake f;f.pre.common_idle=false;f.observations[1].bytes=PackagedOriginal();RunExperiment(Command::kInspect,f);EXPECT_EQ(f.reads,2);EXPECT_EQ(f.writes,0);}
TEST(W1Gate, PendingPermanentlyBlocksApply) {Fake f;f.pending=true;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);}
TEST(W1Gate, BaselineFailurePreventsWrite) {Fake f;f.observations[0].complete=false;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,0);}
TEST(W1Gate, BackupFailurePreventsCheckpointAndWrite) {Fake f;f.save_ok=false;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,0);EXPECT_FALSE(f.pending);}
TEST(W1Gate, PendingFailurePreventsWrite) {Fake f;f.checkpoint_ok=false;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,0);}
TEST(W1Flow, ExactReadbackVerifiedOnce) {Fake f;auto r=RunExperiment(Command::kApply,f);EXPECT_EQ(r.outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(r.exit_code,10);EXPECT_EQ(f.writes,1);EXPECT_EQ(f.reads,2);EXPECT_TRUE(f.pending);}
TEST(W1Flow, DurablePendingBeforeTransportAndFreshReadback) {Fake f;RunExperiment(Command::kApply,f);EXPECT_EQ(f.events,(std::vector<std::string>{"preflight","spawn-read-0","reap-read","backup-fsync-reread","pending-fsync","spawn-write","reap-write","spawn-read-1","reap-read","verified"}));}
TEST(W1Flow, SendFailureNoRetry) {Fake f;f.wr.error="send failure";RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,1);EXPECT_EQ(f.reads,2);}
TEST(W1Flow, TimeoutNoRetryAfterReap) {Fake f;f.wr.timed_out=true;f.wr.exit_code=-1;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,1);EXPECT_EQ(f.reads,2);}
TEST(W1Flow, AmbiguousWriteFreshReadback) {Fake f;f.wr.report.outcome=ProcessOutcome::kAmbiguous;EXPECT_EQ(RunExperiment(Command::kApply,f).outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(f.writes,1);}
TEST(W1Flow, UnreapedWorkerNoOverlap) {Fake f;f.wr.reaped=false;auto r=RunExperiment(Command::kApply,f);EXPECT_EQ(f.reads,1);EXPECT_EQ(r.outcome,"REVIEW_REQUIRED");}
TEST(W1Flow, ReadbackFailureNeverRollsBack) {Fake f;f.observations[1].complete=false;auto r=RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,1);EXPECT_EQ(r.outcome,"REVIEW_REQUIRED");}
TEST(W1Flow, MismatchNeverRollsBack) {Fake f;f.observations[1].bytes={1,2};auto r=RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,1);EXPECT_EQ(r.outcome,"REVIEW_REQUIRED");EXPECT_TRUE(f.pending);}
TEST(W1Flow, OriginalReadbackUnchangedReview) {Fake f;f.observations[1].bytes=PackagedOriginal();EXPECT_EQ(RunExperiment(Command::kApply,f).outcome,"REVIEW_REQUIRED");EXPECT_EQ(f.writes,1);}
TEST(W1Flow, FailedVerifiedJournalRemainsPending) {Fake f;f.verify_ok=false;EXPECT_EQ(RunExperiment(Command::kApply,f).outcome,"REVIEW_REQUIRED");EXPECT_TRUE(f.pending);}
TEST(W1Rollback, RequiresPendingAndBackup) {Fake f;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);f.pending=true;f.backup=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);}
TEST(W1Rollback, ExplicitOnceAndSeparateReadback) {Fake f;f.pending=true;f.observations[0].bytes=PackagedProposal();f.observations[1].bytes=PackagedOriginal();auto r=RunExperiment(Command::kRollback,f);EXPECT_EQ(r.outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(f.writes,1);EXPECT_EQ(f.reads,2);RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,1);}
TEST(W1Rollback, AlreadyOriginalNoWrite) {Fake f;f.pending=true;auto r=RunExperiment(Command::kRollback,f);EXPECT_EQ(r.outcome,"ALREADY_ORIGINAL_REVIEW");EXPECT_EQ(f.writes,0);}
TEST(W1Rollback, ReadFailureBlocksEvenOptIn) {Fake f;f.pending=true;f.observations[0].complete=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);}
class W1JournalTest:public testing::Test {
 protected:
 std::string dir;
 void SetUp() override{char path[]="/tmp/guacamole-w1-XXXXXX";auto* p=mkdtemp(path);ASSERT_NE(p,nullptr);dir=p;}
 void TearDown() override{std::filesystem::remove_all(dir);}
 int Open(){return open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);}
 void File(const char* name,const std::string& bytes,mode_t mode=0600){int f=open((dir+"/"+name).c_str(),O_WRONLY|O_CREAT|O_EXCL,mode);ASSERT_GE(f,0);ASSERT_EQ(write(f,bytes.data(),bytes.size()),static_cast<ssize_t>(bytes.size()));close(f);}
};
TEST_F(W1JournalTest, DurableBackupPendingSurviveNewInstance) {
 {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));ASSERT_TRUE(j.Checkpoint(false));ASSERT_TRUE(j.EnterWorker(false));ASSERT_TRUE(j.Verify(false));}
 Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());EXPECT_TRUE(j.Has("korea_sel_db.pending"));std::vector<uint8_t> b;EXPECT_TRUE(j.Backup(&b));EXPECT_EQ(b,PackagedOriginal());EXPECT_FALSE(j.Checkpoint(false));EXPECT_FALSE(j.EnterWorker(false));
}
TEST_F(W1JournalTest, UnknownEntryBlocks) {File("unexpected","");Journal j(Open(),getuid());EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, WrongModeBlocks) {File("lock","",0644);Journal j(Open(),getuid());EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, WrongOwnerBlocks) {Journal j(Open(),getuid()+1);EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, SymlinkBlocks) {ASSERT_EQ(symlink("/dev/null",(dir+"/lock").c_str()),0);Journal j(Open(),getuid());EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, TornPendingBlocks) {File("korea_sel_db.pending","partial");Journal j(Open(),getuid());EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, LabelMismatchBlocks) {File("lock","");Journal j(Open(),getuid(),[](int){return false;});EXPECT_FALSE(j.Lock());}
TEST_F(W1JournalTest, LockExcludesSecondCoordinator) {Journal a(Open(),getuid()),b(Open(),getuid());ASSERT_TRUE(a.Lock());EXPECT_FALSE(b.Lock());}
TEST_F(W1JournalTest, RollbackCheckpointAndWorkerSingleUse) {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));ASSERT_TRUE(j.Checkpoint(false));ASSERT_TRUE(j.Checkpoint(true));ASSERT_TRUE(j.EnterWorker(true));EXPECT_FALSE(j.Checkpoint(true));EXPECT_FALSE(j.EnterWorker(true));EXPECT_TRUE(j.Has("korea_sel_db.pending"));}
}

namespace {
std::string Cache(const std::string& time="100") {
 std::string b="version=1\nboot=boot-one\ntime="+time+"\nselected=Common/NONE\nauto0=mode=3,flex=true\nauto1=mode=0,flex=false\nhash="+std::string(kSourceHash)+"\nprofiles=1\nreview=1\n";
 return b+"checksum="+Hash({b.begin(),b.end()})+"\n";
}
TEST(W1Status, ValidCacheIsObservationOnly) {auto r=ParseStatus(Cache(),"boot-one",101);EXPECT_TRUE(r.valid);EXPECT_EQ(r.age_seconds,1);EXPECT_TRUE(r.review);EXPECT_NE(r.Diagnostic().find("production=BLOCKED_MAPPING_VALIDATION"),std::string::npos);}
TEST(W1Status, OldBootUnknown) {EXPECT_FALSE(ParseStatus(Cache(),"boot-two",101).valid);}
TEST(W1Status, StaleUnknown) {EXPECT_FALSE(ParseStatus(Cache(),"boot-one",401).valid);}
TEST(W1Status, FutureUnknown) {EXPECT_FALSE(ParseStatus(Cache(),"boot-one",99).valid);}
TEST(W1Status, CorruptionUnknown) {auto s=Cache();s[0]='x';EXPECT_FALSE(ParseStatus(s,"boot-one",101).valid);}
TEST(W1Status, OverflowAndInvalidTimeUnknown) {EXPECT_FALSE(ParseStatus(Cache("999999999999999999999"),"boot-one",101).valid);EXPECT_FALSE(ParseStatus(Cache("x"),"boot-one",101).valid);}
}

namespace {
Fake RecoveryFake(bool common=false) {
 Fake f;f.pending=true;f.pre.common_idle=common;
 f.observations[0].bytes=PackagedProposal();f.observations[1].bytes=PackagedOriginal();return f;
}
TEST(W1C1, ApplyStillRejectsSktNone) {Fake f;f.pre.common_idle=false;RunExperiment(Command::kApply,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);}
TEST(W1C1, RollbackPermitsKnownNonCommonNone) {auto f=RecoveryFake();EXPECT_EQ(RunExperiment(Command::kRollback,f).outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(f.writes,1);}
TEST(W1C1, RollbackPermitsCommonNone) {auto f=RecoveryFake(true);EXPECT_EQ(RunExperiment(Command::kRollback,f).outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(f.writes,1);}
TEST(W1C1, RollbackRejectsUnknownActive) {auto f=RecoveryFake();f.pre.active_known=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);}
TEST(W1C1, RollbackRejectsAnyPending) {auto f=RecoveryFake();f.pre.pending_none=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);}
TEST(W1C1, RollbackRejectsProfileMismatch) {auto f=RecoveryFake();f.pre.profiles=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);}
TEST(W1C1, RollbackRejectsEitherAutoMismatch) {for(bool main:{false,true}){auto f=RecoveryFake();if(main)f.pre.slot0=false;else f.pre.slot1=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);}}
TEST(W1C1, RollbackRetainsAllOtherGates) {for(int i=0;i<6;++i){auto f=RecoveryFake();bool* flags[]={&f.pre.root,&f.pre.debug,&f.pre.context,&f.pre.auto_evidence,&f.pre.artifact,&f.pre.journal};*flags[i]=false;RunExperiment(Command::kRollback,f);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);}}
TEST(W1C1, RollbackWrongConfirmationZeroCalls) {auto f=RecoveryFake();RunExperiment(ParseCommand({"rollback-korea-sel-db-once","--confirm",kApplyConfirmation}),f);EXPECT_TRUE(f.events.empty());}
TEST_F(W1JournalTest, ExactBackupReusedWithoutReplacementOrWrite) {
 Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());const auto b=PackagedOriginal();ASSERT_TRUE(j.SaveBefore(b));
 struct stat before{},after{};ASSERT_EQ(stat((dir+"/korea_sel_db.before").c_str(),&before),0);
 ASSERT_TRUE(j.SaveBefore(b));ASSERT_EQ(stat((dir+"/korea_sel_db.before").c_str(),&after),0);
 EXPECT_EQ(before.st_ino,after.st_ino);EXPECT_EQ(before.st_size,after.st_size);
 EXPECT_EQ(before.st_mtim.tv_sec,after.st_mtim.tv_sec);EXPECT_EQ(before.st_mtim.tv_nsec,after.st_mtim.tv_nsec);
 EXPECT_EQ(before.st_ctim.tv_sec,after.st_ctim.tv_sec);EXPECT_EQ(before.st_ctim.tv_nsec,after.st_ctim.tv_nsec);
 std::vector<uint8_t> saved;ASSERT_TRUE(j.Backup(&saved));EXPECT_EQ(saved,b);EXPECT_FALSE(j.Has("korea_sel_db.pending"));
}
TEST_F(W1JournalTest, DifferentFreshBytesCannotReuseBackup) {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));EXPECT_FALSE(j.SaveBefore(PackagedProposal()));auto changed=PackagedOriginal();changed[0]^=1;EXPECT_FALSE(j.SaveBefore(changed));}
TEST_F(W1JournalTest, TornBeforeRejected) {File("korea_sel_db.before","partial");Journal j(Open(),getuid());EXPECT_FALSE(j.SaveBefore(PackagedOriginal()));}
TEST_F(W1JournalTest, WrongHashBeforeRejected) {auto b=PackagedOriginal();b[0]^=1;File("korea_sel_db.before",{b.begin(),b.end()});Journal j(Open(),getuid());EXPECT_FALSE(j.SaveBefore(PackagedOriginal()));}
TEST_F(W1JournalTest, SymlinkBeforeRejected) {ASSERT_EQ(symlink("/dev/null",(dir+"/korea_sel_db.before").c_str()),0);Journal j(Open(),getuid());EXPECT_FALSE(j.SaveBefore(PackagedOriginal()));}
TEST_F(W1JournalTest, HardlinkedBeforeRejected) {
 Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));
 const auto other=dir+"-hardlink";ASSERT_EQ(link((dir+"/korea_sel_db.before").c_str(),other.c_str()),0);
 EXPECT_FALSE(j.SaveBefore(PackagedOriginal()));EXPECT_EQ(unlink(other.c_str()),0); // test fixture cleanup only
}
TEST_F(W1JournalTest, WrongModeBeforeRejected) {auto b=PackagedOriginal();File("korea_sel_db.before",{b.begin(),b.end()});ASSERT_EQ(chmod((dir+"/korea_sel_db.before").c_str(),0644),0);Journal j(Open(),getuid());EXPECT_FALSE(j.SaveBefore(b));}
TEST_F(W1JournalTest, WrongOwnerExpectationRejectsBefore) {auto b=PackagedOriginal();File("korea_sel_db.before",{b.begin(),b.end()});Journal j(Open(),getuid()+1);EXPECT_FALSE(j.SaveBefore(b));}
TEST_F(W1JournalTest, ExistingBeforeLabelMismatchRejected) {
 {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));}
 Journal j(Open(),getuid(),[](int fd){struct stat st{};return fstat(fd,&st)==0&&st.st_size==0;});
 EXPECT_FALSE(j.SaveBefore(PackagedOriginal()));
}
struct JournalFake:Fake {
 explicit JournalFake(Journal& j):journal(j){}
 Journal& journal;int saves=0;
 bool HasApplyPending() override{return journal.Has("korea_sel_db.pending");}
 bool SaveBefore(const std::vector<uint8_t>& b) override{++saves;return journal.SaveBefore(b);}
 bool Checkpoint(bool rollback) override{return journal.Checkpoint(rollback);}
};
TEST_F(W1JournalTest, OrphanExactBeforeNewCoordinatorCanCheckpointAndProceed) {
 {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));ASSERT_FALSE(j.Has("korea_sel_db.pending"));}
 Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());JournalFake f(j);
 EXPECT_EQ(RunExperiment(Command::kApply,f).outcome,"VERIFIED_REVIEW_REQUIRED");EXPECT_EQ(f.saves,1);EXPECT_EQ(f.writes,1);EXPECT_TRUE(j.Has("korea_sel_db.pending"));
}
TEST_F(W1JournalTest, RetainedPendingBlocksBeforeSaveOnNewCoordinator) {
 {Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());ASSERT_TRUE(j.SaveBefore(PackagedOriginal()));ASSERT_TRUE(j.Checkpoint(false));}
 Journal j(Open(),getuid());ASSERT_TRUE(j.Lock());JournalFake f(j);RunExperiment(Command::kApply,f);
 EXPECT_EQ(f.saves,0);EXPECT_EQ(f.writes,0);EXPECT_EQ(f.reads,0);EXPECT_TRUE(j.Has("korea_sel_db.pending"));
}
}
