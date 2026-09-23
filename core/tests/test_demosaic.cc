// 디모자이크 품질: 에뮬레이터의 노이즈 없는 Bayer → finish() 결과를, 모자이크 전 정답 RGB에 같은 CCM·톤을
// 적용한 8bit 영상과 비교 (PSNR). bilinear vs Malvar-He-Cutler.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "burstpipe/finish.h"
#include "sensor_emu.h"

static double psnr8(const bp::Image<uint8_t>& a, const std::vector<uint8_t>& ref, int W, int H, int border) {
  double se = 0; long n = 0;
  for (int y = border; y < H - border; ++y)
    for (int x = border; x < W - border; ++x)
      for (int c = 0; c < 3; ++c) { double d = (double)a.at(4 * x + c, y) - ref[((size_t)y * W + x) * 3 + c]; se += d * d; ++n; }
  return 10 * std::log10(255.0 * 255.0 / (se / n));
}

int main() {
  bp::ThreadPool pool(4);
  for (int cfa : {0, 2}) {
    bp::emu::EmuParams ep; ep.scene = bp::emu::Scene::kStatic; ep.width = 1024; ep.height = 768; ep.frames = 1; ep.cfa = cfa;
    bp::emu::SensorEmu emu(ep);
    const int W = ep.width, H = ep.height;
    bp::Buffer<uint16_t> bayer(W, H);
    emu.render(0, bayer.img, false, pool);
    std::vector<float> truth((size_t)W * H * 3);
    emu.render_rgb_truth(0, truth.data(), pool);

    bp::FinishParams fp;
    bp::ToneLut lut(fp);
    const float* C = emu.meta().ccm;
    std::vector<uint8_t> ref((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
      const float r = truth[3 * i], g = truth[3 * i + 1], b = truth[3 * i + 2];
      ref[3 * i] = lut(std::max(0.f, C[0] * r + C[1] * g + C[2] * b));
      ref[3 * i + 1] = lut(std::max(0.f, C[3] * r + C[4] * g + C[5] * b));
      ref[3 * i + 2] = lut(std::max(0.f, C[6] * r + C[7] * g + C[8] * b));
    }
    double ps[2];
    for (int k = 0; k < 2; ++k) {
      fp.demosaic = k == 0 ? bp::Demosaic::kBilinear : bp::Demosaic::kMalvar;
      bp::Buffer<uint8_t> rgba(W * 4, H);
      bp::Buffer<float> q((W / 4) * 3, H / 4);
      bp::Arena scratch((size_t)W * H * 4 + (1 << 20));
      bp::finish(bayer.img, emu.meta(), fp, pool, scratch, rgba.img, q.img);
      ps[k] = psnr8(rgba.img, ref, W, H, 4);
    }
    std::printf("cfa=%d PSNR vs truth: bilinear %.2f dB, malvar %.2f dB (+%.2f)\n", cfa, ps[0], ps[1], ps[1] - ps[0]);
    assert(ps[1] > ps[0] + 1.0);
  }
  std::puts("test_demosaic OK");
  return 0;
}
