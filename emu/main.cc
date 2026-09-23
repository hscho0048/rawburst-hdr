// burstpipe_emu: Galaxy C55 RAW 버스트 덤프(앱 Task 1의 출력)를 흉내 낸 합성 버스트를 만든다.
//   burstpipe_emu --out bursts/emu_static --scene static [--size full|half|small|WxH] [--frames 8] ...
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "sensor_emu.h"

static const char* kUsage =
    "burstpipe_emu --out DIR --scene static|motion|lowlight|portrait\n"
    "              [--size full|half|small|WxH] [--frames 8] [--cfa 0..3] [--seed 1] [--shake 2.5] [--iso N]\n"
    "              [--no-noise] [--clean] [--threads T]\n"
    "  full = 4080x3060 (C55 50MP Quad-Bayer 2x2 binned RAW16 가정), half = 2040x1528, small = 1024x768\n"
    "  출력: frame_NN.raw16, meta.txt (앱 덤프와 동일 형식), truth.txt, [인물] mask_emu.bin alpha_gt_q.pgm, [--clean] clean_NN.raw16\n";

int main(int argc, char** argv) {
  bp::emu::EmuParams p;
  std::string out;
  bool clean = false, have_scene = false;
  int threads = (int)std::max(1u, std::thread::hardware_concurrency());
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (!std::strcmp(argv[i], "--out")) out = next();
    else if (!std::strcmp(argv[i], "--scene")) have_scene = bp::emu::parse_scene(next(), p.scene);
    else if (!std::strcmp(argv[i], "--size")) {
      std::string s = next();
      if (s == "full") { p.width = 4080; p.height = 3060; }
      else if (s == "half") { p.width = 2040; p.height = 1528; }
      else if (s == "small") { p.width = 1024; p.height = 768; }
      else if (std::sscanf(s.c_str(), "%dx%d", &p.width, &p.height) != 2) { std::fputs(kUsage, stderr); return 2; }
    }
    else if (!std::strcmp(argv[i], "--frames")) p.frames = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--cfa")) p.cfa = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--seed")) p.seed = (uint32_t)std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--shake")) p.shake_px = (float)std::atof(next().c_str());
    else if (!std::strcmp(argv[i], "--iso")) p.iso = std::atoi(next().c_str());
    else if (!std::strcmp(argv[i], "--threads")) threads = std::max(1, std::atoi(next().c_str()));
    else if (!std::strcmp(argv[i], "--no-noise")) p.noise = false;
    else if (!std::strcmp(argv[i], "--clean")) clean = true;
    else { std::fputs(kUsage, stderr); return 2; }
  }
  if (out.empty() || !have_scene || p.cfa < 0 || p.cfa > 3) { std::fputs(kUsage, stderr); return 2; }
  std::string err;
  if (!bp::emu::write_burst(out, p, clean, threads, &err)) { std::printf("emu failed: %s\n", err.c_str()); return 1; }
  bp::emu::SensorEmu emu(p);
  std::printf("%s: %s %dx%d frames=%d cfa=%d iso=%d noise=(%g,%g)%s\n", out.c_str(), bp::emu::scene_name(p.scene),
              p.width, p.height, p.frames, p.cfa, emu.meta().frames[0].iso, emu.meta().noise_a, emu.meta().noise_b,
              p.noise ? "" : " [no noise]");
  return 0;
}
