#pragma once
#include <vector>
#include "burstpipe/align.h"
#include "burstpipe/burst.h"
#include "burstpipe/thread_pool.h"

namespace bp {

// 초해상도형 합성 (Wronski et al. 2019 "Handheld Multi-Frame Super-Resolution"의 단순화판):
// 디모자이크와 합성을 한 번에. 출력 픽셀마다 모든 프레임의 raw 샘플을 소수 이동까지 반영한 위치에 놓고,
// 색 채널별 가우시안 커널 × 타일 강건성 가중치로 누적 → 풀해상도 선형 RGB (WB 후, finish의 lin 규약).
// 커널은 ref gray 구조 텐서 기반 이방성 (엣지 방향으로 길게). 기본값은 에뮬 탐색(test_superres BP_SR_SWEEP=1) 최적:
// 손떨림 +0.58 dB vs 공간 합성+Malvar, 삼각대 −0.67 dB (서브픽셀 다양성이 없으면 이득이 없다).
struct SrParams {
  float sigma_g = 0.7f, sigma_rb = 0.8f;  // raw px (평탄 영역 값)
  int radius = 2;                         // 이웃 샘플 탐색 반경 (raw px)
  float k = 2.5f;                         // 강건성: 합성과 같은 노이즈 기대치 배수
  // 이방성 커널 (Wronski §4): ref gray의 구조 텐서로 엣지 방향 σ 늘리고(k_stretch) 가로 방향 줄임(k_shrink)
  bool anisotropic = true;
  float k_stretch = 3.0f, k_shrink = 0.35f;
  float edge_d = 0.04f;                   // 엣지 강도 정규화 (sqrt(λ1) 분모, gray raw DN 비율)
};

// fields는 AlignParams::subpixel=true로 만든 것 (fx, fy 사용). rgb: (W*3, H) 할당돼 있어야 함.
void merge_superres(const Burst& b, int ref, const std::vector<MotionField>& fields, const SrParams& p, ThreadPool& pool,
                    Image<float>& rgb);

}  // namespace bp
