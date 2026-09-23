#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace bp {

// ADPF 성능 힌트 세션 (APerformanceHint, API 33). 스케줄러/DVFS에 "이 스레드들은 목표 T ns 안에 끝나야 한다"를 알린다.
// dlsym으로 런타임 해석 → minSdk 30 빌드에서도 동작, 없으면 create()가 nullptr.
class PerfSession {
 public:
  virtual ~PerfSession() = default;
  static std::unique_ptr<PerfSession> create(const std::vector<int32_t>& tids, int64_t target_ns);
  virtual void report(int64_t actual_ns) = 0;
  virtual void update_target(int64_t target_ns) = 0;
};

// AThermal_getThermalHeadroom(forecast) (API 31): 0 = 여유, 1.0 = 심한 스로틀 시작점. 사용 불가면 음수.
float thermal_headroom(int forecast_seconds);
// AThermal_getCurrentThermalStatus (API 30): 0 NONE … 6 SHUTDOWN. 사용 불가면 −1.
int thermal_status();

// 발열 기반 프레임 수 정책 (설계문서 7장 L7): 헤드룸이 높을수록 N을 줄여 셔터→JPEG 지연을 유지.
struct ThermalPolicy {
  int n_max = 8;
  float h_mid = 0.85f;   // 이상이면 n_mid
  int n_mid = 5;
  float h_high = 0.95f;  // 이상이면 n_low
  int n_low = 4;
  int frames_for(float headroom) const {
    if (headroom < 0) return n_max;
    return headroom >= h_high ? n_low : headroom >= h_mid ? n_mid : n_max;
  }
};

// 지연 피드백 거버너: 헤드룸 API가 없는 기기용 (Galaxy C55 실측: 헤드룸 NaN, Thermal Status는 스로틀 중에도 0).
// 실제로 움직이는 신호 = 처리 시간. EMA가 목표의 +10%를 넘으면 N−1, −20% 아래면 N+1.
// 합성 비용은 대략 N에 비례하므로 N을 바꾼 직후 EMA를 그 비율로 보정해 연속 조정(진동)을 막는다.
struct LatencyGovernor {
  double target_ms = 450;
  int n_min = 4, n_max = 8, n = 8;
  double ema = -1;
  int frames() const { return n; }
  void report(double proc_ms) {
    ema = ema < 0 ? proc_ms : 0.7 * ema + 0.3 * proc_ms;
    if (ema > target_ms * 1.10 && n > n_min) { ema *= (double)(n - 1) / n; --n; }
    else if (ema < target_ms * 0.80 && n < n_max) { ema *= (double)(n + 1) / n; ++n; }
  }
};

}  // namespace bp
