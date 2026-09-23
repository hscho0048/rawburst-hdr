#pragma once
#include <cstdint>
#include <vector>
#include "burstpipe/image.h"
#include "burstpipe/thread_pool.h"

namespace bp {

struct Pyramid { std::vector<Image<uint16_t>> levels; };  // [0]=gray 원본

void bayer_to_gray(const Image<uint16_t>& bayer, Image<uint16_t>& gray);  // gray: (w/2,h/2) 할당돼 있어야 함
void downsample2(const Image<uint16_t>& in, Image<uint16_t>& out);         // out: (w/2,h/2)
void build_pyramid(const Image<uint16_t>& gray, int nlevels, Arena& arena, Pyramid& out);
int select_reference(const std::vector<Pyramid>& pyrs, int consider);

// subpixel: 최하단에서 SAD 곡면 등각 직선 맞춤으로 소수 이동 추정 (fx, fy). 초해상도 합성(superres)만 사용 —
//           Bayer 평면 보간 합성에는 오히려 해로웠다 (measurements.md 5.2).
struct AlignParams { int tile = 16; int nlevels = 4; int search_coarse = 4; int search_fine = 2; bool subpixel = false; };

struct MotionField {
  int tiles_x = 0, tiles_y = 0, tile = 16;
  std::vector<int16_t> dx, dy;   // gray 픽셀. alt(x+dx,y+dy) ≈ ref(x,y)
  std::vector<float> err;        // 타일 평균 |diff| (raw gray 단위)
  std::vector<float> fx, fy;     // 서브픽셀 보정 [-0.5,0.5] gray px (AlignParams::subpixel). 총 이동 = dx + fx
  int idx(int tx, int ty) const { return ty * tiles_x + tx; }
  void resize(int tx, int ty, int t) {
    tiles_x = tx; tiles_y = ty; tile = t;
    dx.assign((size_t)tx * ty, 0); dy.assign((size_t)tx * ty, 0); err.assign((size_t)tx * ty, 0.f);
    fx.assign((size_t)tx * ty, 0.f); fy.assign((size_t)tx * ty, 0.f);
  }
};

uint32_t tile_sad_scalar(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile);
uint32_t tile_sad_neon(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile);  // __ARM_NEON 아니면 scalar 호출
inline uint32_t tile_sad(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile) {
#if defined(__ARM_NEON) && !defined(BP_NO_NEON)
  return tile_sad_neon(a, sa, b, sb, tile);
#else
  return tile_sad_scalar(a, sa, b, sb, tile);
#endif
}

void align_frame(const Pyramid& ref, const Pyramid& alt, const AlignParams& p, ThreadPool& pool, MotionField& out);

}  // namespace bp
