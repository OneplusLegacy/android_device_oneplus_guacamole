#include "auto_experiment_journal.h"
#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
using namespace guacamole_pdc;
namespace {
class AutoJournal : public testing::Test {
 protected:
  char path[64]="/tmp/guacamole-auto-journal-XXXXXX";
  int dir=-1;
  void SetUp() override { ASSERT_NE(mkdtemp(path),nullptr);dir=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);ASSERT_GE(dir,0); }
  void TearDown() override {
    for(const char* name:{"slot0.pending","lock","other.final","target"})unlinkat(dir,name,0);
    close(dir);rmdir(path);
  }
  void Create(const char* name) { int fd=openat(dir,name,O_CREAT|O_WRONLY|O_EXCL|O_CLOEXEC,0600);ASSERT_GE(fd,0);close(fd); }
};
}
TEST_F(AutoJournal, DurableMarkerSurvivesNewInstanceAndIsNeverAutoCleared) {
  {
    AutoExperimentJournal journal(dup(dir),geteuid());ASSERT_TRUE(journal.AcquireClean());
    ASSERT_TRUE(journal.RecordPending());EXPECT_TRUE(journal.PendingDurable());
    EXPECT_FALSE(journal.RecordPending());
  }
  AutoExperimentJournal fresh(dup(dir),geteuid());EXPECT_FALSE(fresh.AcquireClean());
  struct stat st{};EXPECT_EQ(fstatat(dir,"slot0.pending",&st,AT_SYMLINK_NOFOLLOW),0);EXPECT_GT(st.st_size,0);
}
TEST_F(AutoJournal, EmptyOrUnrecognizedFinalNeverMeansCompleted) {
  Create("slot0.pending");AutoExperimentJournal journal(dup(dir),geteuid());EXPECT_FALSE(journal.AcquireClean());
}
TEST_F(AutoJournal, UnknownDirectoryEntryBlocks) {
  Create("other.final");AutoExperimentJournal journal(dup(dir),geteuid());EXPECT_FALSE(journal.AcquireClean());
}
TEST_F(AutoJournal, ExclusiveLockPreventsOverlapBeforeCheckpoint) {
  AutoExperimentJournal first(dup(dir),geteuid());ASSERT_TRUE(first.AcquireClean());
  AutoExperimentJournal second(dup(dir),geteuid());EXPECT_FALSE(second.AcquireClean());
}
TEST_F(AutoJournal, BadModeOrOwnerBlocked) {
  ASSERT_EQ(fchmod(dir,0755),0);
  AutoExperimentJournal bad_mode(dup(dir),geteuid());EXPECT_FALSE(bad_mode.AcquireClean());
  ASSERT_EQ(fchmod(dir,0700),0);
  AutoExperimentJournal bad_owner(dup(dir),geteuid()+1);EXPECT_FALSE(bad_owner.AcquireClean());
}
TEST_F(AutoJournal, SymlinkLockBlocked) {
  Create("target");ASSERT_EQ(symlinkat("target",dir,"lock"),0);
  AutoExperimentJournal journal(dup(dir),geteuid());EXPECT_FALSE(journal.AcquireClean());
}
TEST_F(AutoJournal, RecordWithoutAcquisitionOrInvalidDirectoryBlocked) {
  AutoExperimentJournal journal(dup(dir),geteuid());EXPECT_FALSE(journal.RecordPending());
  AutoExperimentJournal missing(-1,geteuid());EXPECT_FALSE(missing.AcquireClean());EXPECT_FALSE(missing.RecordPending());
}
TEST_F(AutoJournal, FailedAcquisitionDoesNotGrantDurablePermit) {
  Create("slot0.pending");AutoExperimentJournal journal(dup(dir),geteuid());
  EXPECT_FALSE(journal.AcquireClean());EXPECT_FALSE(journal.PendingDurable());EXPECT_FALSE(journal.RecordPending());
}
