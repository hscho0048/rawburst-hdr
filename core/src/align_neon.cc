#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "burstpipe/align.h"

namespace bp {

// tile은 8의 배수. 행마다 uint16 절대차(vabdq)를 u32 4레인에 쌍합 누적(vpadalq).
// 16×16 타일 최대합 = 256 × 65535 < 2^32 이므로 오버플로 없음.
uint32_t tile_sad_neon(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile) {
  uint32x4_t acc = vdupq_n_u32(0);
  for (int y = 0; y < tile; ++y, a += sa, b += sb)
    for (int x = 0; x < tile; x += 8)
      acc = vpadalq_u16(acc, vabdq_u16(vld1q_u16(a + x), vld1q_u16(b + x)));
  return vaddvq_u32(acc);
}

}  // namespace bp
#endif
