#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "burstpipe/align.h"
#include "burstpipe/merge.h"

// x64에서는 neon 이름이 scalar를 호출하므로 항상 통과 — 의미 있는 실행은 arm64(기기 또는 에뮬레이터)에서.
int main() {
#if defined(__ARM_NEON)
  std::puts("NEON path: ON");
#else
  std::puts("NEON path: OFF (scalar fallback, trivially equal)");
#endif
  std::srand(11);
  const int W = 64, H = 40;
  bp::Buffer<uint16_t> a(W, H), b(W, H), c(W, H), d(W, H);
  for (int i = 0; i < W * H; ++i) { a.v[i] = std::rand() % 4096; b.v[i] = std::rand() % 4096; c.v[i] = std::rand() % 4096; d.v[i] = std::rand() % 4096; }
  for (int i = 0; i < W * H; i += 7) a.v[i] = 65535;  // 극값
  // tile_sad: 16, 32 — 정수는 bit-exact
  for (int t : {8, 16, 32})
    assert(bp::tile_sad_neon(a.img.row(3) + 5, W, b.img.row(2) + 7, W, t) == bp::tile_sad_scalar(a.img.row(3) + 5, W, b.img.row(2) + 7, W, t));
  // merge_row: 꼬리(37) 포함, alt 하나 nullptr
  const int tw = 37;
  const uint16_t* alts[3] = {b.img.row(1), nullptr, d.img.row(4)};
  float w[3] = {0.7f, 0.5f, 1.0f}, win[64];
  for (int i = 0; i < 64; ++i) win[i] = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (i + 0.5f) / 64);
  float n1[64] = {0}, d1[64] = {0}, n2[64] = {0}, d2[64] = {0};
  bp::merge_row_scalar(a.img.row(0), alts, w, 3, win, 0.8f, tw, n1, d1);
  bp::merge_row_neon(a.img.row(0), alts, w, 3, win, 0.8f, tw, n2, d2);
  float maxerr = 0;
  for (int i = 0; i < tw; ++i) maxerr = std::max({maxerr, std::fabs(n1[i] - n2[i]) / std::max(1.f, n1[i]), std::fabs(d1[i] - d2[i])});
  std::printf("merge_row neon vs scalar rel err %g\n", maxerr);
  assert(maxerr < 1e-5f);
  std::puts("test_neon_eq OK");
  return 0;
}
