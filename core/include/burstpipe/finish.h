#pragma once
#include "burstpipe/burst.h"
#include "burstpipe/thread_pool.h"

namespace bp {

enum class Demosaic { kBilinear, kMalvar };
// kMalvar: Malvar-He-Cutler 2004 (5×5 선형, 색 간 그래디언트 보정). 에뮬 정답 대비 PSNR은 test_demosaic.
// ltm: 로컬 톤매핑 (Mertens 노출 융합, HDR+ 방식). 1/4 해상도 휘도에서 합성 노출 2장(×1, ×ltm_boost)을
//      well-exposedness 가중 Laplacian 피라미드로 융합 → 픽셀당 선형 게인 맵 → 풀해상도에 쌍선형으로 적용.
struct FinishParams {
  float ev_gain = 2.8f; float white_point = 4.0f; Demosaic demosaic = Demosaic::kMalvar;
  bool ltm = false; float ltm_boost = 4.f; float ltm_max_gain = 4.f;
};

// t = lin*ev_gain ∈ [0, white_point] → Reinhard extended → sRGB 8bit
struct ToneLut {
  float ev_gain, white_point, inv_step;
  uint8_t lut[4096];
  explicit ToneLut(const FinishParams& p);
  uint8_t operator()(float lin) const {
    float t = lin * ev_gain * inv_step;
    int i = t <= 0 ? 0 : t >= 4095.f ? 4095 : (int)t;
    return lut[i];
  }
};

// bayer(W,H) → rgba(W*4,H) 표시용 + rgb_lin_q((W/4)*3, H/4) 선형(CCM 후, ev_gain 전), 4x4 평균. 둘 다 할당돼 있어야 함.
// RGB float 이미지 규약: Image<float>{w*3, h}, 픽셀 x의 채널 c = row(y)[3*x+c]. RGBA8은 w*4.
void finish(const Image<uint16_t>& bayer, const BurstMeta& m, const FinishParams& p, ThreadPool& pool,
            Arena& scratch, Image<uint8_t>& rgba, Image<float>& rgb_lin_q);
// Mertens 게인 맵: 1/4 해상도 선형 RGB(qw*3) → gain(qw×qh) ∈ [1, ltm_max_gain]
void local_tone_gain(const Image<float>& rgb_q, const FinishParams& p, Arena& scratch, Image<float>& gain);
// 이미 디모자이크된 풀해상도 선형 RGB(W*3, WB 후) → CCM → 톤 → rgba + rgb_lin_q (초해상도 합성 출력용)
void finish_rgb(const Image<float>& rgb, const BurstMeta& m, const FinishParams& p, ThreadPool& pool, Image<uint8_t>& rgba,
                Image<float>& rgb_lin_q);
// 선형 RGB(w*3) → RGBA8(w*4). 보케 블러 결과 표시용.
void rgb_lin_to_rgba8(const Image<float>& rgb, const ToneLut& lut, ThreadPool& pool, Image<uint8_t>& rgba);

}  // namespace bp
