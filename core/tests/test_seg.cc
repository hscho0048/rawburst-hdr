#include <cassert>
#include <cstdio>
#include <vector>
#include "burstpipe/seg.h"

// 센서 방향 왕복: 센서 좌표의 밝은 점 → bayer_to_rgb256(정립 회전) → "마스크 = R 채널" → mask256_to_sensor → 원위치
static int argmax(const std::vector<float>& v, int stride) {
  int best = 0;
  for (int i = 0; i < 256 * 256; ++i) if (v[(size_t)i * stride] > v[(size_t)best * stride]) best = i;
  return best;
}

int main() {
  // rotate256 기본: 시계방향 90°에서 (x=10,y=20) → (235,10)
  std::vector<float> a(256 * 256, 0.f), b(256 * 256, 0.f);
  a[20 * 256 + 10] = 1.f;
  bp::rotate256(a.data(), b.data(), 1, 90);
  assert(b[10 * 256 + 235] == 1.f);

  const int W = 512, H = 512;
  bp::Buffer<uint16_t> bayer(W, H);
  for (auto& v : bayer.v) v = 64;
  const int sx = 100, sy = 40;  // 센서 좌표 (2×2 슈퍼픽셀 → 256 격자 (50,20))
  for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) bayer.img.at(sx + dx, sy + dy) = 1000;
  for (int deg : {0, 90, 180, 270}) {
    bp::BurstMeta m; m.width = W; m.height = H; m.orientation = deg;
    for (float& v : m.black_level) v = 64;
    std::vector<float> rgb(256 * 256 * 3), mask(256 * 256);
    bp::bayer_to_rgb256(bayer.img, m, 1.f, rgb.data());
    const int up = argmax(rgb, 3);
    for (int i = 0; i < 256 * 256; ++i) mask[i] = rgb[(size_t)i * 3];
    bp::mask256_to_sensor(mask.data(), deg);
    const int back = argmax(mask, 1);
    std::printf("orientation %3d: upright (%d,%d) -> sensor (%d,%d)\n", deg, up % 256, up / 256, back % 256, back / 256);
    assert(back % 256 == sx / 2 && back / 256 == sy / 2);
  }
  // 90°: 센서 (50,20) → 정립 (255-20, 50) = (235,50)
  std::puts("test_seg OK");
  return 0;
}
