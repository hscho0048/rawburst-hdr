#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include "burstpipe/perf_hint.h"
#include "burstpipe/pipeline.h"
#include "burstpipe/power.h"
#include "burstpipe/seg.h"

static const char* kUsage =
    "burstpipe_cli --in DIR --out out.ppm [--mask mask.bin] [--frames N] [--threads T] [--cpus 4,5,6,7]\n"
    "              [--json timings.json] [--dump-merged merged.raw16] [--dump-weights w.pgm] [--dump-alpha a.pgm]\n"
    "              [--ev 2.8] [--radius 12] [--k 2.5] [--repeat R]\n"
    "              [--seg-model PATH --delegate cpu|gpu|npu|emu] [--seg-emu-latency MS] [--seg-cpu C] [--gpu-blur]\n"
    "              [--adpf TARGET_MS] [--thermal-policy] [--csv per_iter.csv]   (연속 --repeat 측정용)\n"
    "              [--seconds S] [--seg-only] [--idle S] [--power-json w.json --label L]   (전력 측정 구간, docs/power.md)\n"
    "  --delegate emu: --seg-model 자리에 256x256 float 마스크(mask_emu.bin)를 받는 에뮬레이션 세그멘터\n"
    "  --seconds S: 웜업 1회 뒤 S초 이상 반복한 구간을 잰다 (연료게이지 갱신 주기보다 길게). --repeat 무시\n"
    "  --seg-only : 합성 없이 세그 추론만 반복 (frame 0 → 256² 입력은 구간 밖에서 1회 준비)\n"
    "  --idle S   : 아무것도 안 하고 S초 — 같은 조건의 idle 기준선 구간 (--in 불필요)\n";

static std::vector<int> parse_ints(const std::string& s) {
  std::vector<int> v;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    if (j > i) v.push_back(std::atoi(s.substr(i, j - i).c_str()));
    i = j + 1;
  }
  return v;
}

static bool write_pgm8_float(const std::string& path, const float* v, int w, int h) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; ++i) {
    float x = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
    std::fputc((int)(x * 255.f + 0.5f), fp);
  }
  std::fclose(fp);
  return true;
}

// 전력 측정 구간 기록 (boottime ns). sysfs가 읽히면 여기서 적분하고, 아니면 앱 PowerService CSV로 PC에서 적분한다.
struct PowerWindow {
  std::string label, mode;
  int64_t t0 = 0, t1 = 0;
  int iters = 0;
  std::vector<double> iter_ms;
  double cpu_c0 = 0, cpu_c1 = 0, skin_c0 = 0, skin_c1 = 0;
  std::string skin_zone;
  void begin() { cpu_c0 = bp::read_cpu_max_c(); skin_c0 = bp::read_skin_c(&skin_zone); t0 = bp::boottime_ns(); }
  void end() { t1 = bp::boottime_ns(); cpu_c1 = bp::read_cpu_max_c(); skin_c1 = bp::read_skin_c(); }
};

static double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

static void write_power_json(const std::string& path, const PowerWindow& w, const std::string& args, const bp::ClockMonitor& cm,
                             bp::BatterySampler& bs, const std::string& timings_json) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) { std::printf("write %s failed\n", path.c_str()); return; }
  const double sec = (w.t1 - w.t0) * 1e-9;
  std::string note;
  const double e = bs.ok() ? bs.energy_j(w.t0, w.t1, &note) : NAN;
  auto mn = [&] { return w.iter_ms.empty() ? 0.0 : *std::min_element(w.iter_ms.begin(), w.iter_ms.end()); };
  auto mx = [&] { return w.iter_ms.empty() ? 0.0 : *std::max_element(w.iter_ms.begin(), w.iter_ms.end()); };
  std::fprintf(f, "{\"label\":\"%s\",\"mode\":\"%s\",\"args\":\"%s\",\"t0_ns\":%lld,\"t1_ns\":%lld,\"seconds\":%.3f,"
               "\"iters\":%d,\"ms_per_iter\":%.3f,\"iter_ms_median\":%.3f,\"iter_ms_min\":%.3f,\"iter_ms_max\":%.3f,"
               "\"cpu_max_c\":[%.1f,%.1f],\"skin_c\":[%.1f,%.1f],\"skin_zone\":\"%s\",\"clocks\":%s,",
               w.label.c_str(), w.mode.c_str(), args.c_str(), (long long)w.t0, (long long)w.t1, sec, w.iters,
               w.iters ? sec * 1e3 / w.iters : 0.0, median(w.iter_ms), mn(), mx(),
               w.cpu_c0, w.cpu_c1, w.skin_c0, w.skin_c1, w.skin_zone.c_str(), cm.json().c_str());
  if (bs.ok()) std::fprintf(f, "\"sysfs_power\":{\"source\":\"%s\",\"gross_j\":%.4f,\"note\":\"%s\"},", bs.source().c_str(), e, note.c_str());
  else std::fprintf(f, "\"sysfs_power\":{\"source\":null,\"why\":\"%s\"},", bs.why().c_str());
  std::fprintf(f, "\"timings_last\":%s}\n", timings_json.empty() ? "null" : timings_json.c_str());
  std::fclose(f);
  std::printf("power window %s: %.2f s, %d iters, %.1f ms/iter -> %s\n", w.label.c_str(), sec, w.iters,
              w.iters ? sec * 1e3 / w.iters : 0.0, path.c_str());
}

int main(int argc, char** argv) {
  std::string in, out, mask, json, dump_merged, dump_weights, dump_alpha, seg_model;
  int frames = 0, repeat = 1;
  double seg_emu_latency = 0, adpf_target_ms = 0;
  bool thermal_policy = false;
  std::string csv, power_json, label;
  double seconds = 0, idle_s = 0;
  bool seg_only = false;
  std::string args;
  for (int i = 1; i < argc; ++i) { if (i > 1) args += ' '; args += argv[i]; }
  bool have_delegate = false;
  bp::Delegate del = bp::Delegate::kCpu;
  bp::PipelineParams p;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (!std::strcmp(argv[i], "--in")) in = next();
    else if (!std::strcmp(argv[i], "--out")) out = next();
    else if (!std::strcmp(argv[i], "--mask")) mask = next();
    else if (!std::strcmp(argv[i], "--json")) json = next();
    else if (!std::strcmp(argv[i], "--dump-merged")) dump_merged = next();
    else if (!std::strcmp(argv[i], "--dump-weights")) dump_weights = next();
    else if (!std::strcmp(argv[i], "--dump-alpha")) dump_alpha = next();
    else if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--threads")) p.threads = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--repeat")) repeat = std::max(1, std::atoi(next().c_str()));
    else if (!std::strcmp(argv[i], "--ev")) p.finish.ev_gain = (float)std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--radius")) p.bokeh.radius = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--k")) p.merge.k = (float)std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--cpus")) p.cpus = parse_ints(next());
    else if (!std::strcmp(argv[i], "--seg-model")) seg_model = next();
    else if (!std::strcmp(argv[i], "--seg-emu-latency")) seg_emu_latency = std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--seg-cpu")) p.seg_cpu = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--gpu-blur")) p.gpu_blur = true;
    else if (!std::strcmp(argv[i], "--ltm")) p.finish.ltm = true;
    else if (!std::strcmp(argv[i], "--merge")) {
      const std::string mm = next();
      p.merge.mode = mm == "wiener" ? bp::MergeMode::kWiener : mm == "superres" ? bp::MergeMode::kSuperRes : bp::MergeMode::kSpatial;
    }
    else if (!std::strcmp(argv[i], "--wiener-c")) p.merge.wiener_c = (float)std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--demosaic")) p.finish.demosaic = next() == "bilinear" ? bp::Demosaic::kBilinear : bp::Demosaic::kMalvar;
    else if (!std::strcmp(argv[i], "--adpf")) adpf_target_ms = std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--thermal-policy")) thermal_policy = true;
    else if (!std::strcmp(argv[i], "--csv")) csv = next();
    else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--idle")) idle_s = std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--seg-only")) seg_only = true;
    else if (!std::strcmp(argv[i], "--power-json")) power_json = next();
    else if (!std::strcmp(argv[i], "--label")) label = next();
    else if (!std::strcmp(argv[i], "--delegate")) {
      have_delegate = bp::parse_delegate(next(), del);
      if (!have_delegate) { std::fputs(kUsage, stderr); return 2; }
    } else { std::fputs(kUsage, stderr); return 2; }
  }
  bp::BatterySampler bsamp(50);
  bsamp.start();
  bp::ClockMonitor clk({0, 4, 7});
  if (idle_s > 0) {  // idle 기준선: 같은 프로세스 구조(샘플러·클럭 스레드)로 아무 일도 하지 않는다
    PowerWindow w; w.label = label.empty() ? "idle" : label; w.mode = "idle";
    clk.start(); w.begin();
    std::this_thread::sleep_for(std::chrono::milliseconds((int64_t)(idle_s * 1000)));
    w.end(); clk.stop(); bsamp.stop();
    if (!power_json.empty()) write_power_json(power_json, w, args, clk, bsamp, "");
    return 0;
  }
  if (in.empty() || (out.empty() && !seg_only) || p.threads < 1) { std::fputs(kUsage, stderr); return 2; }

  bp::BurstMeta meta;
  if (!bp::load_meta(in + "/meta.txt", meta)) { std::puts("meta.txt load failed"); return 1; }
  int n = frames > 0 ? std::min(frames, (int)meta.frames.size()) : (int)meta.frames.size();
  n = std::min(n, bp::kMaxFrames);
  bp::Arena frame_arena((size_t)meta.width * meta.height * 2 * n + ((size_t)n << 7) + (1 << 20));
  bp::Burst b;
  if (!bp::load_burst(in, frame_arena, b, n)) { std::puts("burst load failed"); return 1; }

  std::vector<float> mask256;
  if (!mask.empty()) {
    mask256.resize(256 * 256);
    std::ifstream f(mask, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(mask256.data()), 256 * 256 * 4)) { std::puts("mask load failed"); return 1; }
  }

  bp::Pipeline pipe(meta.width, meta.height, n, p);
  if (p.gpu_blur) std::printf("gpu blur: %s\n", pipe.gpu_status().c_str());

  std::unique_ptr<bp::Segmenter> seg;
  if (!seg_model.empty()) {
    double init_ms = 0;
    if (del == bp::Delegate::kEmu) {
      auto t0 = std::chrono::steady_clock::now();
      seg = bp::Segmenter::create_emulated(seg_model, seg_emu_latency);
      init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    } else {
      seg = bp::Segmenter::create(seg_model, del, 4, &init_ms);
    }
    std::printf("segmenter %s [%s]: %s (init %.0f ms)\n", seg_model.c_str(), bp::delegate_name(del),
                seg ? "ok" : "FAILED", init_ms);
  }

  if (seg_only) {  // E1: 세그 추론만. 입력 준비는 구간 밖에서 1회
    if (!seg) { std::puts("--seg-only needs --seg-model"); return 1; }
    std::vector<float> rgb(256 * 256 * 3), m(256 * 256);
    bp::bayer_to_rgb256(b.frames[0], meta, p.finish.ev_gain, rgb.data());
    seg->run(rgb.data(), m.data());  // 웜업
    PowerWindow w; w.label = label.empty() ? "seg" : label; w.mode = "seg_only";
    clk.start(); w.begin();
    do {
      auto a = std::chrono::steady_clock::now();
      if (!seg->run(rgb.data(), m.data())) { std::puts("seg run failed"); return 1; }
      w.iter_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
      ++w.iters;
    } while ((bp::boottime_ns() - w.t0) * 1e-9 < seconds);
    w.end(); clk.stop(); bsamp.stop();
    std::printf("seg-only %s: %d iters, median %.2f ms\n", bp::delegate_name(del), w.iters, median(w.iter_ms));
    if (!power_json.empty()) write_power_json(power_json, w, args, clk, bsamp, "");
    return 0;
  }

  const float* mp = mask256.empty() ? nullptr : mask256.data();
  bp::Timings t;
  const bp::PipelineOutput* o = nullptr;
  std::unique_ptr<bp::PerfSession> adpf;
  if (adpf_target_ms > 0) {
    adpf = bp::PerfSession::create(pipe.pool().tids(), (int64_t)(adpf_target_ms * 1e6));
    std::printf("adpf: %s (target %.0f ms, %zu threads)\n", adpf ? "session ok" : "unavailable", adpf_target_ms, pipe.pool().tids().size());
  }
  bp::ThermalPolicy policy; policy.n_max = n;
  FILE* cf = csv.empty() ? nullptr : std::fopen(csv.c_str(), "w");
  if (cf) std::fprintf(cf, "iter,frames,total_ms,wall_ms,headroom,status,cpu7_mhz,cpu4_mhz\n");
  float headroom = -1.f;
  auto read_mhz = [](int cpu) {
    char path[96]; std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE* f = std::fopen(path, "r"); long v = 0;
    if (f) { if (std::fscanf(f, "%ld", &v) != 1) v = 0; std::fclose(f); }
    return (int)(v / 1000);
  };
  PowerWindow pw; pw.label = label.empty() ? "burst" : label; pw.mode = "burst";
  if (seconds > 0) {  // 웜업 1회는 구간 밖
    bp::Timings tw;
    pipe.run(b, mp, seg.get(), tw);
    repeat = 1 << 30;
    clk.start(); pw.begin();
  }
  for (int r = 0; r < repeat; ++r) {  // 마지막 회차만 보고 (웜업 제외). --csv면 회차별 기록
    const float h = bp::thermal_headroom(10);
    if (h >= 0) headroom = h;  // 1초 안에 다시 부르면 NaN → 직전 값 유지
    bp::Burst bb = b;
    if (thermal_policy) { const int nn = policy.frames_for(headroom); bb.frames.resize(nn); bb.meta.frames.resize(nn); }
    t = bp::Timings{};
    auto w0 = std::chrono::steady_clock::now();
    o = &pipe.run(bb, mp, seg.get(), t);
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    if (adpf) adpf->report((int64_t)(wall * 1e6));
    if (cf) std::fprintf(cf, "%d,%zu,%.2f,%.2f,%.4f,%d,%d,%d\n", r, bb.frames.size(), t.total(), wall, headroom,
                         bp::thermal_status(), read_mhz(7), read_mhz(4));
    if (seconds > 0) {
      pw.iter_ms.push_back(wall); ++pw.iters;
      if ((bp::boottime_ns() - pw.t0) * 1e-9 >= seconds) break;
    }
  }
  if (seconds > 0) { pw.end(); clk.stop(); }
  bsamp.stop();
  if (cf) std::fclose(cf);

  std::printf("%dx%d frames=%d ref=%d threads=%d mean_weight=%.3f bokeh=%d scratch=%zu/%zuMB persistent=%zuMB\n",
              meta.width, meta.height, n, o->ref, p.threads, o->merge_stats.mean_weight, (int)o->bokeh,
              pipe.scratch_peak() >> 20, pipe.scratch_capacity() >> 20, pipe.persistent_capacity() >> 20);
  for (auto& s : t.ms) std::printf("  %-22s %8.1f ms\n", s.first.c_str(), s.second);
  std::printf("  %-22s %8.1f ms\n", "total", t.total());
  if (!bp::write_ppm8(out, o->rgba)) { std::printf("write %s failed\n", out.c_str()); return 1; }
  if (!dump_merged.empty()) bp::write_raw16(dump_merged, o->merged);
  if (!dump_weights.empty() && !o->merge_weights.empty())
    write_pgm8_float(dump_weights, o->merge_weights.data(), o->weights_w, o->weights_h);
  if (!dump_alpha.empty() && !o->alpha.empty()) write_pgm8_float(dump_alpha, o->alpha.data, o->alpha.w, o->alpha.h);
  if (!json.empty()) { std::ofstream f(json); f << t.json() << "\n"; }
  if (seconds > 0 && !power_json.empty()) write_power_json(power_json, pw, args, clk, bsamp, t.json());
  return 0;
}
