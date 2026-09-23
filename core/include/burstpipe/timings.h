#pragma once
#include <chrono>
#include <string>
#include <utility>
#include <vector>
#if defined(BP_ANDROID)
#include <android/trace.h>
#endif

namespace bp {

struct Timings {
  std::vector<std::pair<std::string, double>> ms;  // 기록 순서 유지
  void add(const std::string& name, double v) { ms.emplace_back(name, v); }
  // 임계 경로 합. 병렬 스레드에서 잰 값("*_parallel")은 wall time에 포함되지 않으므로 뺀다.
  double total() const {
    double s = 0;
    for (auto& p : ms)
      if (p.first.size() < 9 || p.first.compare(p.first.size() - 9, 9, "_parallel") != 0) s += p.second;
    return s;
  }
  std::string json() const {
    std::string s = "{";
    for (size_t i = 0; i < ms.size(); ++i) {
      if (i) s += ",";
      s += "\"" + ms[i].first + "\":" + std::to_string(ms[i].second);
    }
    return s + "}";
  }
};

class ScopedStage {
 public:
  ScopedStage(Timings& t, const char* name) : t_(t), name_(name), t0_(std::chrono::steady_clock::now()) {
#if defined(BP_ANDROID)
    ATrace_beginSection(name);
#endif
  }
  ~ScopedStage() {
#if defined(BP_ANDROID)
    ATrace_endSection();
#endif
    auto dt = std::chrono::steady_clock::now() - t0_;
    t_.add(name_, std::chrono::duration<double, std::milli>(dt).count());
  }
 private:
  Timings& t_;
  const char* name_;
  std::chrono::steady_clock::time_point t0_;
};

}  // namespace bp

#define BP_CAT2(a, b) a##b
#define BP_CAT(a, b) BP_CAT2(a, b)
#define BP_STAGE(timings, name) ::bp::ScopedStage BP_CAT(_bp_stage_, __LINE__)(timings, name)
