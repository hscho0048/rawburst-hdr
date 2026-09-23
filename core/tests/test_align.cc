#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "burstpipe/align.h"
#include "burstpipe/thread_pool.h"

// 랜덤 텍스처를 (sx,sy)만큼 이동한 alt를 만들고 모션이 (sx,sy)로 복원되는지 확인
int main() {
  const int W = 512, H = 384, sx = 3, sy = -2;
  bp::Buffer<uint16_t> ref(W, H), alt(W, H);
  std::srand(1);
  // 8x8 블록마다 랜덤 밝기 (2D 특징) + 노이즈.
  // 주의: ((x/8)*73 + (y/8)*151) % 11 같은 식은 (32,16)px 주기 격자라 거친 단에서 가짜 정합이 생긴다.
  std::vector<int> blk((W / 8) * (H / 8));
  for (int& v : blk) v = std::rand() % 11;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      ref.img.at(x, y) = (uint16_t)(1500 + 300 * blk[(y / 8) * (W / 8) + x / 8] + std::rand() % 200);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      int rx = x - sx, ry = y - sy;  // alt(x,y) = ref(x-sx, y-sy)  ⇒ alt(x+sx,y+sy)=ref(x,y)
      rx = rx < 0 ? 0 : rx >= W ? W - 1 : rx; ry = ry < 0 ? 0 : ry >= H ? H - 1 : ry;
      alt.img.at(x, y) = (uint16_t)(ref.img.at(rx, ry) + std::rand() % 50);
    }
  bp::Arena arena(64 << 20);
  bp::Pyramid pr, pa;
  bp::build_pyramid(ref.img, 4, arena, pr);
  bp::build_pyramid(alt.img, 4, arena, pa);
  bp::ThreadPool pool(1);
  bp::MotionField f;
  bp::align_frame(pr, pa, bp::AlignParams{}, pool, f);
  int ok = 0, total = 0;
  for (int ty = 1; ty < f.tiles_y - 1; ++ty)      // 가장자리 타일 제외
    for (int tx = 1; tx < f.tiles_x - 1; ++tx) {
      ++total;
      if (f.dx[f.idx(tx, ty)] == sx && f.dy[f.idx(tx, ty)] == sy) ++ok;
    }
  std::printf("align: %d/%d tiles exact\n", ok, total);
  assert(ok * 100 >= total * 95);
  // 멀티스레드 결과 == 1스레드 결과
  bp::ThreadPool pool4(4);
  bp::MotionField f4;
  bp::align_frame(pr, pa, bp::AlignParams{}, pool4, f4);
  assert(f4.dx == f.dx && f4.dy == f.dy && f4.err == f.err);
  // tile_sad 디스패치 == scalar
  assert(bp::tile_sad(ref.img.row(10) + 10, W, alt.img.row(20) + 30, W, 16) ==
         bp::tile_sad_scalar(ref.img.row(10) + 10, W, alt.img.row(20) + 30, W, 16));
  std::puts("test_align OK");
  return 0;
}
