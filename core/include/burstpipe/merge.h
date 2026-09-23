#pragma once
#include <vector>
#include "burstpipe/align.h"
#include "burstpipe/burst.h"
#include "burstpipe/thread_pool.h"

namespace bp {

constexpr int kMaxFrames = 16;

enum class MergeMode { kSpatial, kWiener };
// tile: Bayer 단위, 겹침 stride = tile/2. kWiener: 주파수 영역 (HDR+ §5), wiener_c = 수축 강도
struct MergeParams { float k = 2.5f; int tile = 32; MergeMode mode = MergeMode::kSpatial; float wiener_c = 8.f; };
struct MergeStats { float mean_weight = 0; float expected_diff = 0; };

// 노이즈만으로 기대되는 gray 타일 평균 |ref-alt| (raw 단위)
float expected_diff_from_profile(const BurstMeta& m, float mean_raw);
float expected_diff_from_fields(const std::vector<MotionField>& fields, int ref);
bool noise_profile_plausible(const BurstMeta& m);   // 범위 밖이면 프로파일 무시 (HAL 오류 대비)

// fields[i]는 frames[i]의 모션. fields[ref]는 비어 있어도 됨. merged는 (W,H) 할당돼 있어야 함.
// weights != nullptr 이면 합성 타일 격자(mtx×mty)의 alt 평균 가중치를 기록 (고스트 시각화용).
MergeStats merge_burst(const Burst& b, int ref, const std::vector<MotionField>& fields, const MergeParams& p,
                       ThreadPool& pool, Arena& scratch, Image<uint16_t>& merged, std::vector<float>* weights = nullptr);

MergeStats merge_burst_wiener(const Burst& b, int ref, const std::vector<MotionField>& fields, const MergeParams& p,
                              ThreadPool& pool, Arena& scratch, Image<uint16_t>& merged, std::vector<float>* weights = nullptr);

// 타일 1행(tw 픽셀)을 누적. NEON 버전은 merge_neon.cc. i번째 alt의 행 포인터가 nullptr이면 그 프레임은 이 행에서 제외.
void merge_row_scalar(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                      float win_y, int tw, float* num, float* den);
void merge_row_neon(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                    float win_y, int tw, float* num, float* den);
inline void merge_row(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                      float win_y, int tw, float* num, float* den) {
#if defined(__ARM_NEON) && !defined(BP_NO_NEON)
  merge_row_neon(ref, alts, w, nalt, win_x, win_y, tw, num, den);
#else
  merge_row_scalar(ref, alts, w, nalt, win_x, win_y, tw, num, den);
#endif
}

}  // namespace bp
