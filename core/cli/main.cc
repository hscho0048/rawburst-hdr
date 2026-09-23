#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "burstpipe/perf_hint.h"
#include "burstpipe/pipeline.h"
#include "burstpipe/seg.h"

static const char* kUsage =
    "burstpipe_cli --in DIR --out out.ppm [--mask mask.bin] [--frames N] [--threads T] [--cpus 4,5,6,7]\n"
    "              [--json timings.json] [--dump-merged merged.raw16] [--dump-weights w.pgm] [--dump-alpha a.pgm]\n"
    "              [--ev 2.8] [--radius 12] [--k 2.5] [--repeat R]\n"
    "              [--seg-model PATH --delegate cpu|gpu|npu|emu] [--seg-emu-latency MS] [--seg-cpu C] [--gpu-blur]\n"
    "              [--adpf TARGET_MS] [--thermal-policy] [--csv per_iter.csv]   (연속 --repeat 측정용)\n"
    "  --delegate emu: --seg-model 자리에 256x256 float 마스크(mask_emu.bin)를 받는 에뮬레이션 세그멘터\n";

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

int main(int argc, char** argv) {
  std::string in, out, mask, json, dump_merged, dump_weights, dump_alpha, seg_model;
  int frames = 0, repeat = 1;
  double seg_emu_latency = 0, adpf_target_ms = 0;
  bool thermal_policy = false;
  std::string csv;
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
    else if (!std::strcmp(argv[i], "--merge")) p.merge.mode = next() == "wiener" ? bp::MergeMode::kWiener : bp::MergeMode::kSpatial;
    else if (!std::strcmp(argv[i], "--wiener-c")) p.merge.wiener_c = (float)std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--demosaic")) p.finish.demosaic = next() == "bilinear" ? bp::Demosaic::kBilinear : bp::Demosaic::kMalvar;
    else if (!std::strcmp(argv[i], "--adpf")) adpf_target_ms = std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--thermal-policy")) thermal_policy = true;
    else if (!std::strcmp(argv[i], "--csv")) csv = next();
    else if (!std::strcmp(argv[i], "--delegate")) {
      have_delegate = bp::parse_delegate(next(), del);
      if (!have_delegate) { std::fputs(kUsage, stderr); return 2; }
    } else { std::fputs(kUsage, stderr); return 2; }
  }
  if (in.empty() || out.empty() || p.threads < 1) { std::fputs(kUsage, stderr); return 2; }

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
  }
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
  return 0;
}
