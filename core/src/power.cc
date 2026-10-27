#include "burstpipe/power.h"

#include <dirent.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bp {
namespace {

bool read_long(const std::string& path, long long& v) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  const bool ok = std::fscanf(f, "%lld", &v) == 1;
  std::fclose(f);
  return ok;
}

std::string read_line(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return "";
  char buf[128] = {0};
  if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
  std::fclose(f);
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

// thermal_zone 목록 (type, temp 경로) — 한 번만 훑는다
struct Zone { std::string type, temp; };
const std::vector<Zone>& zones() {
  static const std::vector<Zone> z = [] {
    std::vector<Zone> v;
    DIR* d = opendir("/sys/class/thermal");
    if (!d) return v;
    while (dirent* e = readdir(d)) {
      if (std::strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
      const std::string base = std::string("/sys/class/thermal/") + e->d_name;
      const std::string t = read_line(base + "/type");
      if (!t.empty()) v.push_back({t, base + "/temp"});
    }
    closedir(d);
    return v;
  }();
  return z;
}

double zone_c(const Zone& z) {
  long long v;
  if (!read_long(z.temp, v)) return NAN;
  // 대부분 m°C. 일부 영역은 °C 정수 → 크기로 구분. 음수(-273000 = 미사용)는 버림
  if (v < -50000) return NAN;
  return std::llabs(v) > 1000 ? v / 1000.0 : (double)v;
}

}  // namespace

int64_t boottime_ns() {
#if defined(CLOCK_BOOTTIME)
  timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

int read_cpu_khz(int cpu, const char* file) {
  char path[96];
  std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, file);
  long long v = 0;
  return read_long(path, v) ? (int)v : 0;
}

double read_thermal_c(const std::string& type) {
  for (auto& z : zones()) if (z.type == type) return zone_c(z);
  return NAN;
}

double read_cpu_max_c() {
  double m = NAN;
  for (auto& z : zones()) {
    if (z.type.find("cpu") == std::string::npos) continue;
    const double c = zone_c(z);
    if (!std::isnan(c) && c > 0 && c < 150 && (std::isnan(m) || c > m)) m = c;
  }
  return m;
}

double read_skin_c(std::string* which) {
  for (const char* t : {"quiet-therm", "quiet_therm", "skin-msm-therm", "xo-therm", "xo_therm"}) {
    const double c = read_thermal_c(t);
    if (!std::isnan(c) && c > 0) { if (which) *which = t; return c; }
  }
  return NAN;
}

// ---------------- BatterySampler ----------------

BatterySampler::BatterySampler(int period_ms) : period_ms_(period_ms) {
  // 후보: battery → bms → 그 밖의 power_supply. current_now·voltage_now 둘 다 읽혀야 채택
  std::vector<std::string> dirs = {"/sys/class/power_supply/battery", "/sys/class/power_supply/bms"};
  if (DIR* d = opendir("/sys/class/power_supply")) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      const std::string p = std::string("/sys/class/power_supply/") + e->d_name;
      if (std::find(dirs.begin(), dirs.end(), p) == dirs.end()) dirs.push_back(p);
    }
    closedir(d);
  }
  for (auto& d : dirs) {
    long long c, v;
    const bool rc = read_long(d + "/current_now", c), rv = read_long(d + "/voltage_now", v);
    if (rc && rv) { cur_path_ = d + "/current_now"; volt_path_ = d + "/voltage_now"; return; }
    why_ += d + (rc ? "" : " current_now:" + std::string(std::strerror(errno))) + "; ";
  }
}

BatterySampler::~BatterySampler() { stop(); }

void BatterySampler::start() {
  if (!ok() || run_) return;
  run_ = true;
  th_ = std::thread([this] {
    while (run_) {
      long long c = 0, v = 0;
      const int64_t t = boottime_ns();
      if (read_long(cur_path_, c) && read_long(volt_path_, v)) {
        std::lock_guard<std::mutex> lk(m_);
        s_.push_back({t, c, v});
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(period_ms_));
    }
  });
}

void BatterySampler::stop() {
  run_ = false;
  if (th_.joinable()) th_.join();
}

std::vector<BatterySampler::Sample> BatterySampler::samples() {
  std::lock_guard<std::mutex> lk(m_);
  return s_;
}

double BatterySampler::energy_j(int64_t t0, int64_t t1, std::string* note) {
  auto s = samples();
  std::vector<Sample> w;
  for (auto& x : s) if (x.t_ns >= t0 && x.t_ns <= t1) w.push_back(x);
  if (w.size() < 2) { if (note) *note = "samples<2"; return NAN; }
  // 단위: |I| 중앙값 > 20000 이면 µA, 아니면 mA. 전압: > 100000 이면 µV, 아니면 mV
  std::vector<long long> ai;
  for (auto& x : w) ai.push_back(std::llabs(x.current_raw));
  std::nth_element(ai.begin(), ai.begin() + ai.size() / 2, ai.end());
  const double i_scale = ai[ai.size() / 2] > 20000 ? 1e-6 : 1e-3;
  const double v_scale = w[0].voltage_raw > 100000 ? 1e-6 : 1e-3;
  // 부호: 이 구간 평균의 부호를 방전 방향으로 본다 (구간 안에서 충전이 섞이면 의미 없음 → 케이블 분리 필수)
  double sum_i = 0;
  for (auto& x : w) sum_i += (double)x.current_raw;
  const double sign = sum_i < 0 ? -1.0 : 1.0;
  double e = 0;
  for (size_t k = 1; k < w.size(); ++k) {
    const double dt = (w[k].t_ns - w[k - 1].t_ns) * 1e-9;
    const double p0 = sign * w[k - 1].current_raw * i_scale * w[k - 1].voltage_raw * v_scale;
    const double p1 = sign * w[k].current_raw * i_scale * w[k].voltage_raw * v_scale;
    e += 0.5 * (p0 + p1) * dt;
  }
  if (note) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "n=%zu i_unit=%s sign=%+.0f", w.size(), i_scale == 1e-6 ? "uA" : "mA", sign);
    *note = buf;
  }
  return e;
}

// ---------------- ClockMonitor ----------------

ClockMonitor::ClockMonitor(std::vector<int> cpus, int period_ms)
    : cpus_(std::move(cpus)), period_ms_(period_ms), sum_(cpus_.size()), n_(cpus_.size()),
      min_(cpus_.size(), 0), cap_min_(cpus_.size(), 0), cap_max_(cpus_.size(), 0) {}

ClockMonitor::~ClockMonitor() { stop(); }

void ClockMonitor::start() {
  if (run_) return;
  run_ = true;
  th_ = std::thread([this] {
    while (run_) {
      for (size_t i = 0; i < cpus_.size(); ++i) {
        const int cur = read_cpu_khz(cpus_[i], "scaling_cur_freq") / 1000;
        const int cap = read_cpu_khz(cpus_[i], "scaling_max_freq") / 1000;
        if (cur <= 0) continue;
        sum_[i] += cur; n_[i] += 1;
        if (!min_[i] || cur < min_[i]) min_[i] = cur;
        if (!cap_min_[i] || cap < cap_min_[i]) cap_min_[i] = cap;
        if (cap > cap_max_[i]) cap_max_[i] = cap;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(period_ms_));
    }
  });
}

void ClockMonitor::stop() {
  run_ = false;
  if (th_.joinable()) th_.join();
}

std::string ClockMonitor::json() const {
  std::string s = "{";
  for (size_t i = 0; i < cpus_.size(); ++i) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s\"cpu%d\":{\"mean_mhz\":%.0f,\"min_mhz\":%d,\"cap_min_mhz\":%d,\"cap_max_mhz\":%d}",
                  i ? "," : "", cpus_[i], n_[i] > 0 ? sum_[i] / n_[i] : 0.0, min_[i], cap_min_[i], cap_max_[i]);
    s += buf;
  }
  return s + "}";
}

}  // namespace bp
