#include "diag_process_worker.h"
#include "hash.h"
#include <algorithm>

namespace guacamole_efs {
namespace {
constexpr std::array<uint8_t, 8> kMagic{'G','D','R','D','0','0','0','1'};
void Put(std::vector<uint8_t>& v, size_t p, uint32_t n) {
  for (int i = 0; i < 4; ++i) v[p + i] = static_cast<uint8_t>(n >> (8 * i));
}
uint32_t Get(const std::vector<uint8_t>& v, size_t p) {
  uint32_t n = 0;
  for (int i = 0; i < 4; ++i) n |= uint32_t(v[p + i]) << (8 * i);
  return n;
}
}
std::vector<uint8_t> EncodeWorkerReport(const WorkerReadReport& r) {
  if (r.pid == 0) return {};
  const auto status = static_cast<uint32_t>(r.status);
  if (status > 2 || (status == 0 && (r.bytes.size() != 32 || !r.teardown_completed)) ||
      (status != 0 && !r.bytes.empty())) return {};
  std::vector<uint8_t> v(kWorkerFrameSize, 0);
  std::copy(kMagic.begin(), kMagic.end(), v.begin());
  std::copy(r.nonce.begin(), r.nonce.end(), v.begin() + 8);
  Put(v, 24, r.pid); Put(v, 28, status); Put(v, 32, r.bytes.size());
  Put(v, 36, r.teardown_completed ? 1 : 0);
  if (status == 0) {
    const auto digest = Sha256(r.bytes.data(), r.bytes.size());
    if (digest != r.sha256) return {};
    std::copy(r.bytes.begin(), r.bytes.end(), v.begin() + 40);
    std::copy(digest.begin(), digest.end(), v.begin() + 72);
  }
  const auto sum = Sha256(v.data(), 104);
  std::copy(sum.begin(), sum.end(), v.begin() + 104);
  return v;
}
bool DecodeWorkerReport(const std::vector<uint8_t>& v, uint32_t pid,
    const WorkerNonce& nonce, WorkerReadReport* out, std::string* error) {
  auto fail = [&](const char* message) { if (error) *error = message; return false; };
  if (!out || v.size() != kWorkerFrameSize) return fail("incomplete/oversized worker frame");
  if (!std::equal(kMagic.begin(), kMagic.end(), v.begin()) ||
      !std::equal(nonce.begin(), nonce.end(), v.begin() + 8) || pid == 0 || Get(v, 24) != pid)
    return fail("worker magic/nonce/pid mismatch");
  const auto sum = Sha256(v.data(), 104);
  if (!std::equal(sum.begin(), sum.end(), v.begin() + 104)) return fail("worker checksum mismatch");
  const auto status = Get(v, 28), count = Get(v, 32), teardown = Get(v, 36);
  if (status > 2 || teardown > 1 || (status == 0 && (count != 32 || teardown != 1)) ||
      (status != 0 && count != 0)) return fail("invalid worker status/length/teardown");
  WorkerReadReport r;
  r.pid = pid; r.nonce = nonce; r.status = static_cast<WorkerReadStatus>(status);
  r.teardown_completed = teardown == 1;
  if (status == 0) {
    r.bytes.assign(v.begin() + 40, v.begin() + 72);
    r.sha256 = Sha256(r.bytes.data(), r.bytes.size());
    if (!std::equal(r.sha256.begin(), r.sha256.end(), v.begin() + 72))
      return fail("worker data hash mismatch");
  } else if (!std::all_of(v.begin() + 40, v.begin() + 104, [](uint8_t b) { return b == 0; })) {
    return fail("failure frame contains success data");
  }
  *out = std::move(r);
  return true;
}
std::string WorkerNonceHex(const WorkerNonce& nonce) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto b : nonce) { result += hex[b >> 4]; result += hex[b & 15]; }
  return result;
}
bool ParseWorkerNonce(const std::string& s, WorkerNonce* nonce) {
  if (!nonce || s.size() != 32) return false;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  WorkerNonce n{};
  for (size_t i = 0; i < n.size(); ++i) {
    const int hi = nibble(s[i*2]), lo = nibble(s[i*2+1]);
    if (hi < 0 || lo < 0) return false;
    n[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  *nonce = n; return true;
}
ProcessReadCycleResult RunDiagProcessCycleReadonly(IReadWorkerLauncher& launcher,
    const std::function<void(const std::string&)>& progress) {
  ProcessReadCycleResult r;
  for (size_t i = 0; i < r.workers.size(); ++i) {
    const auto tag = "worker" + std::to_string(i + 1);
    if (progress) progress(tag + ".spawn_begin");
    r.workers[i] = launcher.Run();
    const auto& w = r.workers[i];
    if (progress) progress(tag + (w.reaped ? ".reaped" : ".not_reaped"));
    if (!w.success || !w.spawned || !w.reaped || w.timed_out || !w.report_valid ||
        w.exit_code != 0 || w.signal != 0 || w.report.status != WorkerReadStatus::kOk ||
        !w.report.teardown_completed || w.report.bytes.size() != 32) return r;
  }
  // PID reuse after reaping is legal. Each successful spawn+exec, nonce and wait
  // establish freshness; numerical PID inequality is not the freshness contract.
  if (r.workers[0].report.nonce == r.workers[1].report.nonce) return r;
  r.exact_equal = r.workers[0].report.bytes == r.workers[1].report.bytes &&
      r.workers[0].report.sha256 == r.workers[1].report.sha256;
  return r;
}
}
