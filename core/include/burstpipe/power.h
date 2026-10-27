#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 전력·에너지 측정 (배터리 전체 기준). Pixel ODPM 같은 레일별 전력이 없는 기기용.
// - BatterySampler: /sys/class/power_supply/*/current_now·voltage_now를 period_ms마다 읽어 P=V×I를 적분.
//   삼성·샤오미 user 빌드는 shell에서 Permission denied → ok()=false. 그때는 앱의 PowerService
//   (BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)가 샘플을 남기고, cli는 구간 경계(boottime ns)만 기록한다.
// - 구간 경계는 CLOCK_BOOTTIME (= Java SystemClock.elapsedRealtimeNanos) → 앱 샘플과 같은 시간축.
namespace bp {

int64_t boottime_ns();

// cpu{n}/cpufreq/<file> (kHz). 실패 시 0
int read_cpu_khz(int cpu, const char* file /*"scaling_cur_freq" 등*/);
// thermal_zone 중 type이 정확히 같은 첫 영역 (°C). 없으면 NaN
double read_thermal_c(const std::string& type);
// type에 "cpu"가 들어간 영역 중 최고값 (°C) — 기기마다 이름이 달라(cpu-1-0 / cpu-1-0-usr) 최대값으로 통일. 없으면 NaN
double read_cpu_max_c();
// 표면 온도 후보(quiet-therm, quiet_therm, skin-msm-therm, xo-therm, xo_therm) 중 처음 읽히는 것. 없으면 NaN
double read_skin_c(std::string* which = nullptr);

class BatterySampler {
 public:
  struct Sample { int64_t t_ns; int64_t current_raw; int64_t voltage_raw; };
  explicit BatterySampler(int period_ms = 50);
  ~BatterySampler();
  bool ok() const { return !cur_path_.empty() && !volt_path_.empty(); }
  const std::string& source() const { return cur_path_; }
  const std::string& why() const { return why_; }  // ok()=false인 이유 (시도한 경로·에러)
  void start();
  void stop();
  std::vector<Sample> samples();
  // [t0,t1] 구간 에너지 (J, 방전 양수). 단위·부호는 값 크기와 방향으로 추정 — 기기별 검증은 docs/power.md
  double energy_j(int64_t t0, int64_t t1, std::string* note = nullptr);
 private:
  int period_ms_;
  std::string cur_path_, volt_path_, why_;
  std::thread th_;
  std::atomic<bool> run_{false};
  std::mutex m_;
  std::vector<Sample> s_;
};

// 구간 동안 클럭·발열을 주기적으로 기록 (period_ms). 평균 클럭과 스로틀(최대 클럭 상한 하락) 여부를 본다.
class ClockMonitor {
 public:
  explicit ClockMonitor(std::vector<int> cpus, int period_ms = 200);
  ~ClockMonitor();
  void start();
  void stop();
  // {"cpu4":{"mean_mhz":..,"min_mhz":..,"cap_min_mhz":..,"cap_max_mhz":..},...}
  std::string json() const;
 private:
  std::vector<int> cpus_;
  int period_ms_;
  std::thread th_;
  std::atomic<bool> run_{false};
  std::vector<double> sum_, n_;
  std::vector<int> min_, cap_min_, cap_max_;
};

}  // namespace bp
