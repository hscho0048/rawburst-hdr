#include "burstpipe/finish.h"
#include <algorithm>
#include <cmath>

namespace bp {

static float srgb_encode(float v) {
  v = std::min(1.f, std::max(0.f, v));
  return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

ToneLut::ToneLut(const FinishParams& p) : ev_gain(p.ev_gain), white_point(p.white_point), inv_step(4095.f / p.white_point) {
  const float W2 = p.white_point * p.white_point;
  for (int i = 0; i < 4096; ++i) {
    float t = i / inv_step;                       // 노출 게인 후 값 [0, W]
    float y = t * (1.f + t / W2) / (1.f + t);     // Reinhard extended: t=W에서 1
    lut[i] = (uint8_t)std::lround(255.f * srgb_encode(y));
  }
}

void finish(const Image<uint16_t>& bayer, const BurstMeta& m, const FinishParams& p, ThreadPool& pool,
            Arena& scratch, Image<uint8_t>& rgba, Image<float>& rgb_lin_q) {
  const int W = bayer.w, H = bayer.h;
  Image<float> lin = scratch.alloc<float>(W, H);
  // 1) 블랙레벨, WB → 정규화 선형. WB 후 1.0에서 클립: G가 포화한 하이라이트에서 R/B만 게인만큼 커져
  //    분홍/마젠타로 변하는 것을 막는다 (포화 = 백색). 1.0 = G 채널 포화점.
  pool.parallel_for(H, [&](int y) {
    const uint16_t* r = bayer.row(y); float* o = lin.row(y);
    for (int x = 0; x < W; ++x) {
      float bl = m.black_at(x, y);
      o[x] = std::min(1.f, std::max(0.f, (r[x] - bl) * m.gain_at(x, y) / (m.white_level - bl)));
    }
  });
  // 2) 디모자이크(bilinear) + CCM + 톤 + 1/4 선형 누적. 4행 단위로 병렬 → 1/4 버퍼 레이스 없음
  const ToneLut lut(p);
  const float* C = m.ccm;
  const int QW = W / 4;
  pool.parallel_for(H / 4, [&](int qy) {
    float* q = rgb_lin_q.row(qy);
    std::fill(q, q + QW * 3, 0.f);
    for (int y = qy * 4; y < qy * 4 + 4; ++y) {
      uint8_t* o = rgba.row(y);
      // 경계는 반사(−1 → +1): 같은 CFA 색 위상을 유지한다. 클램프하면 이웃 색이 섞인다.
      const float* rm = lin.row(y == 0 ? 1 : y - 1);
      const float* r0 = lin.row(y);
      const float* rp = lin.row(y == H - 1 ? H - 2 : y + 1);
      for (int x = 0; x < W; ++x) {
        const int c = kCfaColor[m.cfa][(y & 1) * 2 + (x & 1)];
        const float v = r0[x];
        const int xm = x == 0 ? 1 : x - 1;
        const int xp = x == W - 1 ? W - 2 : x + 1;
        const float hsum = r0[xm] + r0[xp], vsum = rm[x] + rp[x];
        float r, g, b;
        switch (c) {
          case 0: r = v; g = 0.25f * (hsum + vsum); b = 0.25f * (rm[xm] + rm[xp] + rp[xm] + rp[xp]); break;
          case 3: b = v; g = 0.25f * (hsum + vsum); r = 0.25f * (rm[xm] + rm[xp] + rp[xm] + rp[xp]); break;
          case 1: g = v; r = 0.5f * hsum; b = 0.5f * vsum; break;   // Gr: R 행
          default: g = v; r = 0.5f * vsum; b = 0.5f * hsum; break;  // Gb: B 행
        }
        float R = std::max(0.f, C[0] * r + C[1] * g + C[2] * b);
        float G = std::max(0.f, C[3] * r + C[4] * g + C[5] * b);
        float B = std::max(0.f, C[6] * r + C[7] * g + C[8] * b);
        o[4 * x] = lut(R); o[4 * x + 1] = lut(G); o[4 * x + 2] = lut(B); o[4 * x + 3] = 255;
        if ((x >> 2) < QW) { float* qp = q + 3 * (x >> 2); qp[0] += R * (1.f / 16); qp[1] += G * (1.f / 16); qp[2] += B * (1.f / 16); }
      }
    }
  });
}

void rgb_lin_to_rgba8(const Image<float>& rgb, const ToneLut& lut, ThreadPool& pool, Image<uint8_t>& rgba) {
  const int w = rgb.w / 3;
  pool.parallel_for(rgb.h, [&](int y) {
    const float* s = rgb.row(y); uint8_t* o = rgba.row(y);
    for (int x = 0; x < w; ++x) { o[4 * x] = lut(s[3 * x]); o[4 * x + 1] = lut(s[3 * x + 1]); o[4 * x + 2] = lut(s[3 * x + 2]); o[4 * x + 3] = 255; }
  });
}

}  // namespace bp
