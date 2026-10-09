#pragma once
#include "auto_slot0_experiment.h"
namespace guacamole_pdc {
// Opens fixed init-owned root0700 directory; never creates a fallback directory.
int OpenDeviceAutoExperimentDirectory();
// Owns supplied directory fd. Owner injection exists for host tests only; device
// adapter always supplies0. No directory or owner parameter is exposed by CLI.
class AutoExperimentJournal final : public IAutoExperimentState {
 public:
  explicit AutoExperimentJournal(int directory_fd,uint32_t owner=0) : directory_(directory_fd),owner_(owner) {}
  ~AutoExperimentJournal() override;
  AutoExperimentJournal(const AutoExperimentJournal&) = delete;
  AutoExperimentJournal& operator=(const AutoExperimentJournal&) = delete;
  bool AcquireClean() override;
  bool RecordPending() override;
  bool PendingDurable() const override { return durable_ && lock_>=0; }
 private:
  bool CleanDirectory() const;
  int directory_=-1, lock_=-1;
  uint32_t owner_;
  bool acquired_=false, durable_=false, acquire_consumed_=false, record_consumed_=false;
};
}
