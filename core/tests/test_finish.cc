#include <cassert>
#include <cmath>
#include <cstdio>
#include "burstpipe/finish.h"

// 회색 카드: WB 게인을 나눈 raw를 넣으면 WB 후 모든 채널이 0.5 → CCM=I → r≈g≈b
int main() {
  const int W = 64, H = 48;
  for (int cfa = 0; cfa < 4; ++cfa) {
    bp::BurstMeta m; m.width = W; m.height = H; m.cfa = cfa; m.white_level = 1023;
    for (float& v : m.black_level) v = 64;
    m.wb_gains[0] = 2.0f; m.wb_gains[1] = 1.0f; m.wb_gains[2] = 1.0f; m.wb_gains[3] = 1.5f;
    bp::Buffer<uint16_t> bayer(W, H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        bayer.img.at(x, y) = (uint16_t)std::lround(64 + 0.5f * (1023 - 64) / m.gain_at(x, y));
    bp::Buffer<uint8_t> rgba(W * 4, H);
    bp::Buffer<float> q((W / 4) * 3, H / 4);
    bp::Arena scratch(4 << 20);
    bp::ThreadPool pool(2);
    bp::FinishParams p; p.ev_gain = 1.0f;
    bp::finish(bayer.img, m, p, pool, scratch, rgba.img, q.img);
    // 경계 포함 모든 픽셀이 회색이어야 한다 (반사 경계 → CFA 위상 유지)
    int worst = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const uint8_t* px = rgba.img.row(y) + 4 * x;
        worst = std::max({worst, std::abs(px[0] - px[1]), std::abs(px[1] - px[2])});
      }
    const uint8_t* px = rgba.img.row(10) + 4 * 10;
    std::printf("cfa=%d rgb = %d %d %d (worst |r-g|,|g-b| = %d), lin_q = %.3f %.3f %.3f\n", cfa, px[0], px[1], px[2], worst,
                q.img.at(0, 2), q.img.at(1, 2), q.img.at(2, 2));
    assert(worst <= 2);
    assert(std::fabs(q.img.at(3 * 3 + 1, 2) - 0.5f) < 0.02f);
  }
  bp::FinishParams p; p.ev_gain = 1.0f;
  bp::ToneLut lut(p);
  std::printf("lut(0.5)=%d\n", lut(0.5f));
  assert(lut(0.f) == 0 && lut(0.5f) > 150 && lut(0.5f) < 200);  // 0.5 선형 → Reinhard 0.344 → sRGB ≈ 158
  assert(lut(4.0f) == 255);
  std::puts("test_finish OK");
  return 0;
}
