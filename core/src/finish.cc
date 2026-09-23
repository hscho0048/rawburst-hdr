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
  const ToneLut lut(p);
  const float* C = m.ccm;
  const int QW = W / 4, QH = H / 4;
  // 1.5) 로컬 톤매핑: 4×4 블록 CFA 평균 → 1/4 RGB(CCM 후) → Mertens 게인 맵. 행 테이블로 풀해상도 업샘플
  Image<float> gainq;
  std::vector<int> gx0, gx1; std::vector<float> gwx;
  if (p.ltm) {
    Image<float> q0 = scratch.alloc<float>(QW * 3, QH);
    pool.parallel_for(QH, [&](int qy) {
      float* o = q0.row(qy);
      for (int qx = 0; qx < QW; ++qx) {
        float acc[4] = {0, 0, 0, 0}; int cnt[4] = {0, 0, 0, 0};
        for (int dy = 0; dy < 4; ++dy) {
          const int y = 4 * qy + dy; const float* r = lin.row(y);
          for (int dx = 0; dx < 4; ++dx) { const int x = 4 * qx + dx; const int c = kCfaColor[m.cfa][(y & 1) * 2 + (x & 1)]; acc[c] += r[x]; ++cnt[c]; }
        }
        const float rr = acc[0] / cnt[0], gg = (acc[1] + acc[2]) / (cnt[1] + cnt[2]), bb = acc[3] / cnt[3];
        o[3 * qx] = std::max(0.f, C[0] * rr + C[1] * gg + C[2] * bb);
        o[3 * qx + 1] = std::max(0.f, C[3] * rr + C[4] * gg + C[5] * bb);
        o[3 * qx + 2] = std::max(0.f, C[6] * rr + C[7] * gg + C[8] * bb);
      }
    });
    gainq = scratch.alloc<float>(QW, QH);
    local_tone_gain(q0, p, scratch, gainq);
    gx0.resize(W); gx1.resize(W); gwx.resize(W);
    for (int x = 0; x < W; ++x) {
      const float fx = std::max(0.f, (x + 0.5f) / 4.f - 0.5f);
      gx0[x] = std::min((int)fx, QW - 1); gx1[x] = std::min(gx0[x] + 1, QW - 1); gwx[x] = fx - gx0[x];
    }
  }
  // 2) 디모자이크 + CCM + (로컬 게인) + 톤 + 1/4 선형 누적. 4행 단위로 병렬 → 1/4 버퍼 레이스 없음
  pool.parallel_for(H / 4, [&](int qy) {
    float* q = rgb_lin_q.row(qy);
    std::fill(q, q + QW * 3, 0.f);
    thread_local std::vector<float> grow;  // 이 행의 게인 (세로 보간 끝난 1/4 행)
    for (int y = qy * 4; y < qy * 4 + 4; ++y) {
      uint8_t* o = rgba.row(y);
      const float* gr = nullptr;
      if (p.ltm) {
        if ((int)grow.size() < QW) grow.resize(QW);
        const float fy = std::max(0.f, (y + 0.5f) / 4.f - 0.5f);
        const int y0 = std::min((int)fy, QH - 1), y1 = std::min(y0 + 1, QH - 1); const float wy = fy - y0;
        const float* a0 = gainq.row(y0); const float* a1 = gainq.row(y1);
        for (int q = 0; q < QW; ++q) grow[q] = a0[q] + wy * (a1[q] - a0[q]);
        gr = grow.data();
      }
      // 경계는 반사(−k → +k): 같은 CFA 색 위상을 유지한다. 클램프하면 이웃 색이 섞인다.
      auto ry = [&](int yy) { return lin.row(yy < 0 ? -yy : yy >= H ? 2 * H - 2 - yy : yy); };
      const float* rm2 = ry(y - 2); const float* rm = ry(y - 1); const float* r0 = lin.row(y);
      const float* rp = ry(y + 1); const float* rp2 = ry(y + 2);
      const bool malvar = p.demosaic == Demosaic::kMalvar;
      for (int x = 0; x < W; ++x) {
        const int c = kCfaColor[m.cfa][(y & 1) * 2 + (x & 1)];
        const float v = r0[x];
        auto rx = [&](int xx) { return xx < 0 ? -xx : xx >= W ? 2 * W - 2 - xx : xx; };
        const int xm = rx(x - 1), xp = rx(x + 1);
        const float hsum = r0[xm] + r0[xp], vsum = rm[x] + rp[x];
        const float dsum = rm[xm] + rm[xp] + rp[xm] + rp[xp];
        float r, g, b;
        if (!malvar) {
          switch (c) {
            case 0: r = v; g = 0.25f * (hsum + vsum); b = 0.25f * dsum; break;
            case 3: b = v; g = 0.25f * (hsum + vsum); r = 0.25f * dsum; break;
            case 1: g = v; r = 0.5f * hsum; b = 0.5f * vsum; break;   // Gr: R 행
            default: g = v; r = 0.5f * vsum; b = 0.5f * hsum; break;  // Gb: B 행
          }
        } else {
          // Malvar-He-Cutler: 같은 색 ±2 이웃의 라플라시안으로 색 간 그래디언트를 보정
          const int xm2 = rx(x - 2), xp2 = rx(x + 2);
          const float h2 = r0[xm2] + r0[xp2], v2 = rm2[x] + rp2[x];
          const float gx = (4.f * v + 2.f * (hsum + vsum) - (h2 + v2)) * 0.125f;          // G at R/B
          const float cx = (6.f * v + 2.f * dsum - 1.5f * (h2 + v2)) * 0.125f;            // B at R / R at B
          const float hz = (5.f * v + 4.f * hsum - h2 - dsum + 0.5f * v2) * 0.125f;      // 가로 이웃 색 at G
          const float vt = (5.f * v + 4.f * vsum - v2 - dsum + 0.5f * h2) * 0.125f;      // 세로 이웃 색 at G
          switch (c) {
            case 0: r = v; g = gx; b = cx; break;
            case 3: b = v; g = gx; r = cx; break;
            case 1: g = v; r = hz; b = vt; break;   // Gr: R 행, B 열
            default: g = v; r = vt; b = hz; break;  // Gb: B 행, R 열
          }
          r = std::max(0.f, r); g = std::max(0.f, g); b = std::max(0.f, b);
        }
        float R = std::max(0.f, C[0] * r + C[1] * g + C[2] * b);
        float G = std::max(0.f, C[3] * r + C[4] * g + C[5] * b);
        float B = std::max(0.f, C[6] * r + C[7] * g + C[8] * b);
        if (gr) {
          const float k = gr[gx0[x]] + gwx[x] * (gr[gx1[x]] - gr[gx0[x]]);
          R *= k; G *= k; B *= k;
        }
        o[4 * x] = lut(R); o[4 * x + 1] = lut(G); o[4 * x + 2] = lut(B); o[4 * x + 3] = 255;
        if ((x >> 2) < QW) { float* qp = q + 3 * (x >> 2); qp[0] += R * (1.f / 16); qp[1] += G * (1.f / 16); qp[2] += B * (1.f / 16); }
      }
    }
  });
}

// ---- Mertens 노출 융합 (휘도만) ----------------------------------------------------------------
namespace {
// 5탭 이항 [1 4 6 4 1]/16 가우시안 후 ½ 다운샘플 (경계 반사)
void pyr_down(const std::vector<float>& in, int w, int h, std::vector<float>& out, int& ow, int& oh) {
  ow = (w + 1) / 2; oh = (h + 1) / 2;
  std::vector<float> t((size_t)ow * h);
  static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
  auto rf = [](int i, int n) { return i < 0 ? -i : i >= n ? 2 * n - 2 - i : i; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < ow; ++x) { float s = 0; for (int j = -2; j <= 2; ++j) s += k[j + 2] * in[(size_t)y * w + rf(2 * x + j, w)]; t[(size_t)y * ow + x] = s; }
  out.assign((size_t)ow * oh, 0.f);
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) { float s = 0; for (int j = -2; j <= 2; ++j) s += k[j + 2] * t[(size_t)rf(2 * y + j, h) * ow + x]; out[(size_t)y * ow + x] = s; }
}
// 2배 업샘플 (쌍선형) → (w, h)
void pyr_up(const std::vector<float>& in, int iw, int ih, int w, int h, std::vector<float>& out) {
  out.assign((size_t)w * h, 0.f);
  for (int y = 0; y < h; ++y) {
    const float fy = std::min((float)ih - 1, std::max(0.f, (y - 0.5f) / 2.f));
    const int y0 = (int)fy, y1 = std::min(y0 + 1, ih - 1); const float wy = fy - y0;
    for (int x = 0; x < w; ++x) {
      const float fx = std::min((float)iw - 1, std::max(0.f, (x - 0.5f) / 2.f));
      const int x0 = (int)fx, x1 = std::min(x0 + 1, iw - 1); const float wx = fx - x0;
      out[(size_t)y * w + x] = (1 - wy) * ((1 - wx) * in[(size_t)y0 * iw + x0] + wx * in[(size_t)y0 * iw + x1]) +
                               wy * ((1 - wx) * in[(size_t)y1 * iw + x0] + wx * in[(size_t)y1 * iw + x1]);
    }
  }
}
}  // namespace

void local_tone_gain(const Image<float>& rgb_q, const FinishParams& p, Arena&, Image<float>& gain) {
  const int w = gain.w, h = gain.h, n = w * h;
  const ToneLut lut(p);
  // 톤 커브(표시값) 와 그 역 (선형 = lin*ev_gain 공간에서)
  auto tone = [&](float lin) { return lut(lin) / 255.f; };
  std::vector<float> Y(n);
  for (int y = 0; y < h; ++y) {
    const float* s = rgb_q.row(y);
    for (int x = 0; x < w; ++x) Y[(size_t)y * w + x] = 0.2126f * s[3 * x] + 0.7152f * s[3 * x + 1] + 0.0722f * s[3 * x + 2];
  }
  // 합성 노출 2장 (표시값) + well-exposedness 가중치 (Mertens 2007, σ=0.2)
  std::vector<float> e[2], wgt[2];
  const float mult[2] = {1.f, p.ltm_boost};
  for (int k = 0; k < 2; ++k) {
    e[k].resize(n); wgt[k].resize(n);
    for (int i = 0; i < n; ++i) {
      const float v = tone(Y[i] * mult[k]);
      e[k][i] = v;
      wgt[k][i] = std::exp(-(v - 0.5f) * (v - 0.5f) / (2 * 0.2f * 0.2f)) + 1e-4f;
    }
  }
  for (int i = 0; i < n; ++i) { const float s = wgt[0][i] + wgt[1][i]; wgt[0][i] /= s; wgt[1][i] /= s; }
  // 피라미드 융합: Σk Gauss(wk)_l · Lap(ek)_l
  int levels = 1;
  for (int m = std::min(w, h); m > 16; m /= 2) ++levels;
  std::vector<std::vector<float>> fused(levels);
  std::vector<int> lw(levels), lh(levels);
  for (int k = 0; k < 2; ++k) {
    std::vector<float> gi = e[k], gw = wgt[k];
    int cw = w, ch = h;
    for (int l = 0; l < levels; ++l) {
      lw[l] = cw; lh[l] = ch;
      if (fused[l].empty()) fused[l].assign((size_t)cw * ch, 0.f);
      if (l == levels - 1) {  // 최상단: 가우시안 그대로
        for (size_t i = 0; i < gi.size(); ++i) fused[l][i] += gw[i] * gi[i];
        break;
      }
      std::vector<float> ni, nw, up; int nw_, nh_;
      pyr_down(gi, cw, ch, ni, nw_, nh_);
      int t1, t2; pyr_down(gw, cw, ch, nw, t1, t2);
      pyr_up(ni, nw_, nh_, cw, ch, up);
      for (size_t i = 0; i < gi.size(); ++i) fused[l][i] += gw[i] * (gi[i] - up[i]);
      gi.swap(ni); gw.swap(nw); cw = nw_; ch = nh_;
    }
  }
  std::vector<float> img = fused[levels - 1], up;
  for (int l = levels - 2; l >= 0; --l) {
    pyr_up(img, lw[l + 1], lh[l + 1], lw[l], lh[l], up);
    for (size_t i = 0; i < up.size(); ++i) up[i] += fused[l][i];
    img.swap(up);
  }
  // 융합 표시값 F → 선형 게인: tone(Y·g) = F 가 되는 g (톤 LUT 역: 이분 탐색 대신 단조 테이블)
  std::vector<float> inv(256);
  {
    int j = 0;
    for (int v = 0; v < 256; ++v) {
      while (j < 4095 && lut.lut[j] < v) ++j;
      inv[v] = j / lut.inv_step / lut.ev_gain;  // 선형 lin 값 (ev_gain 전)
    }
  }
  for (int y = 0; y < h; ++y) {
    float* g = gain.row(y);
    for (int x = 0; x < w; ++x) {
      const size_t i = (size_t)y * w + x;
      const float F = std::min(1.f, std::max(0.f, img[i]));
      const float fv = F * 255.f; const int v0 = std::min(254, (int)fv); const float fr = fv - v0;
      const float target = inv[v0] + fr * (inv[v0 + 1] - inv[v0]);
      g[x] = std::min(p.ltm_max_gain, std::max(1.f, target / std::max(Y[i], 1e-5f)));
    }
  }
}

void rgb_lin_to_rgba8(const Image<float>& rgb, const ToneLut& lut, ThreadPool& pool, Image<uint8_t>& rgba) {
  const int w = rgb.w / 3;
  pool.parallel_for(rgb.h, [&](int y) {
    const float* s = rgb.row(y); uint8_t* o = rgba.row(y);
    for (int x = 0; x < w; ++x) { o[4 * x] = lut(s[3 * x]); o[4 * x + 1] = lut(s[3 * x + 1]); o[4 * x + 2] = lut(s[3 * x + 2]); o[4 * x + 3] = 255; }
  });
}

}  // namespace bp
