#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "burstpipe/bokeh.h"

int main() {
  const int W = 128, H = 96;
  bp::Buffer<float> I(W, H), p(W, H), q(W, H), tmp(W, H), o(W, H);
  std::srand(3);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { I.img.at(x, y) = (std::rand() % 1000) / 1000.f; p.img.at(x, y) = 0.3f; }
  // 상수 입력의 박스 필터는 경계 포함 상수
  bp::box_filter(p.img, 5, tmp.img, o.img);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) assert(std::fabs(o.img.at(x, y) - 0.3f) < 1e-5f);
  // 상수 p → 가이드와 무관하게 q == p
  bp::Arena scratch(8 << 20);
  bp::guided_filter(I.img, p.img, 8, 1e-3f, scratch, q.img);
  float maxerr = 0;
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) maxerr = std::max(maxerr, std::fabs(q.img.at(x, y) - 0.3f));
  std::printf("guided const: maxerr=%g\n", maxerr);
  assert(maxerr < 1e-3f);
  // 계단 p, 계단 I(같은 위치) → 경계가 유지된다 (블러되지 않음)
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { I.img.at(x, y) = x < W / 2 ? 0.2f : 0.8f; p.img.at(x, y) = x < W / 2 ? 0.f : 1.f; }
  scratch.reset();
  bp::guided_filter(I.img, p.img, 8, 1e-3f, scratch, q.img);
  std::printf("step: q(W/2-1)=%.3f q(W/2)=%.3f\n", q.img.at(W / 2 - 1, H / 2), q.img.at(W / 2, H / 2));
  assert(q.img.at(W / 2 - 1, H / 2) < 0.15f && q.img.at(W / 2, H / 2) > 0.85f);
  // 거친 마스크(경계가 8px 어긋남) + 선명한 가이드 → 경계가 가이드 쪽으로 붙는다
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) p.img.at(x, y) = x < W / 2 + 4 ? 0.f : 1.f;
  scratch.reset();
  bp::guided_filter(I.img, p.img, 8, 1e-3f, scratch, q.img);
  std::printf("snap: q(W/2+1)=%.3f (raw mask 0)\n", q.img.at(W / 2 + 1, H / 2));
  assert(q.img.at(W / 2 + 1, H / 2) > 0.3f);

  // 정규화 디스크 블러: α=1 영역(전경)의 색이 배경으로 번지지 않는다
  bp::Buffer<float> rgb(W * 3, H), alpha(W, H), out(W * 3, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      bool fg = x < W / 2;
      alpha.img.at(x, y) = fg ? 1.f : 0.f;
      for (int c = 0; c < 3; ++c) rgb.img.at(3 * x + c, y) = fg ? 1.f : 0.1f;
    }
  bp::ThreadPool pool(2);
  bp::Arena bs(4 << 20);
  bp::disc_blur_normalized(rgb.img, alpha.img, 6, pool, bs, out.img);
  std::printf("halo: bg next to fg = %.3f (should stay 0.1)\n", out.img.at(3 * (W / 2), H / 2));
  assert(std::fabs(out.img.at(3 * (W / 2), H / 2) - 0.1f) < 1e-4f);
  // 누적합 구현 == 직접 합산 참조 (랜덤 α·색, 경계 포함, 반지름 여러 개)
  {
    std::srand(9);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        alpha.img.at(x, y) = (std::rand() % 3 == 0) ? 1.f : (std::rand() % 1000) / 1000.f * 0.97f;
        for (int c = 0; c < 3; ++c) rgb.img.at(3 * x + c, y) = (std::rand() % 1000) / 250.f;
      }
    bp::Buffer<float> ref(W * 3, H);
    for (int r : {1, 5, 12, 20}) {
      bs.reset();
      bp::disc_blur_normalized(rgb.img, alpha.img, r, pool, bs, out.img);
      bp::disc_blur_direct(rgb.img, alpha.img, r, pool, ref.img);
      float me = 0;
      for (size_t i = 0; i < ref.v.size(); ++i) me = std::max(me, std::fabs(out.v[i] - ref.v[i]));
      std::printf("disc blur prefix vs direct r=%d: max|diff|=%g\n", r, me);
      assert(me < 2e-3f);
    }
  }
  std::puts("test_guided OK");
  return 0;
}
