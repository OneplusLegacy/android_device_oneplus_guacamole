// Production Binder registration. No startup mutation. V2 observation schedules bounded read-only work.
#include "guacamole_efsd_service.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_ibinder_platform.h>

using guacamole_efsd::GuacamoleEfsdService;

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(0);

  std::shared_ptr<GuacamoleEfsdService> service =
      ndk::SharedRefBase::make<GuacamoleEfsdService>();

  AIBinder_setRequestingSid(service->asBinder().get(), true);

  const std::string instance =
      std::string(GuacamoleEfsdService::descriptor) + "/default";
  binder_status_t status =
      AServiceManager_addService(service->asBinder().get(), instance.c_str());
  CHECK_EQ(status, STATUS_OK);

  ABinderProcess_joinThreadPool();
  return EXIT_FAILURE;  // should not reach.
}
