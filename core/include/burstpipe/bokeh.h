#pragma once
#include "burstpipe/image.h"
#include "burstpipe/thread_pool.h"

namespace bp {

struct BokehParams { int radius = 12; int gf_radius = 8; float gf_eps = 1e-3f; float highlight = 3.0f; };

void box_filter(const Image<float>& in, int r, Image<float>& tmp, Image<float>& out);  // 경계: 유효 픽셀 수로 나눔. in==out 허용
void guided_filter(const Image<float>& I, const Image<float>& p, int r, float eps, Arena& scratch, Image<float>& q);
void resize_bilinear(const Image<float>& in, Image<float>& out);          // 단일 채널, out 크기로
void luma_of(const Image<float>& rgb, float gain, Image<float>& y);       // min(1, gain·(0.2126R+0.7152G+0.0722B))
void highlight_boost(Image<float>& rgb, float gain, float strength);      // in-place
// (1-α) 가중 정규화 디스크 블러. 전경(α>0.98)은 계산 생략하고 원본 복사. 경계는 가장자리 복제.
// 행 누적합 구현: 원형 커널 = 행마다 구간합 → 픽셀당 (2r+1)×2 조회 (직접 합산은 ~πr² 탭). scratch: h×(w+1)×4 float.
void disc_blur_normalized(const Image<float>& rgb, const Image<float>& alpha, int radius, ThreadPool& pool, Arena& scratch,
                          Image<float>& out);
// 직접 합산 참조 구현 (정답지, 테스트용)
void disc_blur_direct(const Image<float>& rgb, const Image<float>& alpha, int radius, ThreadPool& pool, Image<float>& out);
// out = α·sharp + (1−α)·upsample(blur_q). out==sharp 허용
void composite(const Image<uint8_t>& sharp, const Image<uint8_t>& blur_q, const Image<float>& alpha_q, ThreadPool& pool,
               Image<uint8_t>& out);

}  // namespace bp
