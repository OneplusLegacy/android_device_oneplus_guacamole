#include "carrier_experiment.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace guacamole_pdc {
CarrierFile ReadCarrierFile(CarrierTarget target) {
  CarrierFile r;
  const auto* profile = CarrierFileProfile(target);
  if (!profile || (target != CarrierTarget::kKt && target != CarrierTarget::kLgu)) {
    r.error = "not a loadable carrier"; return r;
  }
  const std::string path = profile->runtime_path;
  if (path.empty() || path[0] != '/' || profile->expected_size > 1024 * 1024) {
    r.error = "invalid fixed path/size"; return r;
  }
  int dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) { r.error = "root open failed"; return r; }
  size_t start = 1;
  int fd = -1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto component = path.substr(start, end == std::string::npos ? end : end - start);
    if (component.empty() || component == "." || component == "..") break;
    if (end == std::string::npos) {
      fd = openat(dir, component.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      break;
    }
    const int next = openat(dir, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(dir); dir = next;
    if (dir < 0) break;
    start = end + 1;
  }
  if (dir >= 0) close(dir);
  if (fd < 0) { r.error = "fixed path open failed/symlink refused"; return r; }
  struct stat before{}, after{};
  if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
      before.st_size != profile->expected_size) {
    close(fd); r.error = "file type/size/link count invalid"; return r;
  }
  r.bytes.resize(profile->expected_size);
  size_t nread = 0;
  while (nread < r.bytes.size()) {
    const auto n = read(fd, r.bytes.data() + nread, r.bytes.size() - nread);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    nread += static_cast<size_t>(n);
  }
  uint8_t extra = 0;
  const auto tail = read(fd, &extra, 1);
  const bool stable = fstat(fd, &after) == 0 && before.st_dev == after.st_dev &&
      before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
      before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
  close(fd);
  r.verified = nread == r.bytes.size() && tail == 0 && stable && VerifyCarrierBytes(target, r.bytes);
  if (!r.verified) { r.bytes.clear(); r.error = "full size/hash/stability verification failed"; }
  return r;
}
}
