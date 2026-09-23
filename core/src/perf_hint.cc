#include "burstpipe/perf_hint.h"

#if defined(BP_ANDROID)
#include <dlfcn.h>

namespace bp {
namespace {

struct Api {
  // performance_hint.h (API 33)
  void* (*getManager)() = nullptr;
  void* (*createSession)(void*, const int32_t*, size_t, int64_t) = nullptr;
  int (*reportActual)(void*, int64_t) = nullptr;
  int (*updateTarget)(void*, int64_t) = nullptr;
  void (*closeSession)(void*) = nullptr;
  // thermal.h (API 30/31)
  void* (*acquireThermal)() = nullptr;
  float (*headroom)(void*, int) = nullptr;
  int (*status)(void*) = nullptr;
  void* thermal = nullptr;
  Api() {
    void* h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) return;
    getManager = (decltype(getManager))dlsym(h, "APerformanceHint_getManager");
    createSession = (decltype(createSession))dlsym(h, "APerformanceHint_createSession");
    reportActual = (decltype(reportActual))dlsym(h, "APerformanceHint_reportActualWorkDuration");
    updateTarget = (decltype(updateTarget))dlsym(h, "APerformanceHint_updateTargetWorkDuration");
    closeSession = (decltype(closeSession))dlsym(h, "APerformanceHint_closeSession");
    acquireThermal = (decltype(acquireThermal))dlsym(h, "AThermal_acquireManager");
    headroom = (decltype(headroom))dlsym(h, "AThermal_getThermalHeadroom");
    status = (decltype(status))dlsym(h, "AThermal_getCurrentThermalStatus");
    if (acquireThermal) thermal = acquireThermal();
  }
};
Api& api() { static Api a; return a; }

class AdpfSession : public PerfSession {
 public:
  explicit AdpfSession(void* s) : s_(s) {}
  ~AdpfSession() override { if (api().closeSession) api().closeSession(s_); }
  void report(int64_t ns) override { api().reportActual(s_, ns); }
  void update_target(int64_t ns) override { if (api().updateTarget) api().updateTarget(s_, ns); }
 private:
  void* s_;
};

}  // namespace

std::unique_ptr<PerfSession> PerfSession::create(const std::vector<int32_t>& tids, int64_t target_ns) {
  Api& a = api();
  if (!a.getManager || !a.createSession || !a.reportActual || tids.empty()) return nullptr;
  void* m = a.getManager();
  if (!m) return nullptr;
  void* s = a.createSession(m, tids.data(), tids.size(), target_ns);
  if (!s) return nullptr;
  return std::make_unique<AdpfSession>(s);
}

float thermal_headroom(int forecast_seconds) {
  Api& a = api();
  if (!a.thermal || !a.headroom) return -1.f;
  float h = a.headroom(a.thermal, forecast_seconds);
  return h != h ? -1.f : h;  // NaN(호출 간격이 너무 짧음 등) → 사용 불가
}

int thermal_status() {
  Api& a = api();
  return (a.thermal && a.status) ? a.status(a.thermal) : -1;
}

}  // namespace bp

#else

namespace bp {
std::unique_ptr<PerfSession> PerfSession::create(const std::vector<int32_t>&, int64_t) { return nullptr; }
float thermal_headroom(int) { return -1.f; }
int thermal_status() { return -1; }
}  // namespace bp

#endif
