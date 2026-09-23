#if defined(__ARM_NEON)
#include <arm_neon.h>
#include "burstpipe/merge.h"

namespace bp {

// merge를 fp16으로 내리지 않는 이유: raw 10비트 × 가중치 합 최대 8 = 8184까지 누적되는데 fp16 가수는 11비트라
// 4096 이상에서 정수 해상도가 4가 된다. ±2 LSB 양자화 노이즈가 SNR 측정을 갉아먹는다. fp16은 보케 블러에만.
void merge_row_neon(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                    float win_y, int tw, float* num, float* den) {
  float ws_row = 1.f;                       // 행 안에서 일정 (nullptr 여부는 행 단위)
  for (int i = 0; i < nalt; ++i) if (alts[i]) ws_row += w[i];
  const float32x4_t vwy = vdupq_n_f32(win_y), vws = vdupq_n_f32(ws_row);
  int x = 0;
  for (; x + 4 <= tw; x += 4) {
    float32x4_t acc = vcvtq_f32_u32(vmovl_u16(vld1_u16(ref + x)));
    for (int i = 0; i < nalt; ++i) {
      if (!alts[i]) continue;
      // vmlaq_n_f32는 곱·덧셈을 따로 반올림 → scalar(a += w*b)와 같은 순서·같은 결과
      acc = vmlaq_n_f32(acc, vcvtq_f32_u32(vmovl_u16(vld1_u16(alts[i] + x))), w[i]);
    }
    const float32x4_t g = vmulq_f32(vwy, vld1q_f32(win_x + x));
    vst1q_f32(num + x, vmlaq_f32(vld1q_f32(num + x), g, acc));
    vst1q_f32(den + x, vmlaq_f32(vld1q_f32(den + x), g, vws));
  }
  if (x < tw) {
    const uint16_t* tail[kMaxFrames];
    for (int i = 0; i < nalt; ++i) tail[i] = alts[i] ? alts[i] + x : nullptr;
    merge_row_scalar(ref + x, tail, w, nalt, win_x + x, win_y, tw - x, num + x, den + x);
  }
}

}  // namespace bp
#endif
