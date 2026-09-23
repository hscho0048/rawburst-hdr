#include "burstpipe/bokeh.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace bp {

// 수평: 행 누적합, 수직: 러닝섬. 픽셀당 O(1). 수평 패스가 tmp로 먼저 끝나므로 in==out 허용.
void box_filter(const Image<float>& in, int r, Image<float>& tmp, Image<float>& out) {
  const int w = in.w, h = in.h;
  std::vector<float> P(w + 1);
  for (int y = 0; y < h; ++y) {
    const float* s = in.row(y); float* t = tmp.row(y);
    P[0] = 0; for (int x = 0; x < w; ++x) P[x + 1] = P[x] + s[x];
    for (int x = 0; x < w; ++x) { int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r); t[x] = (P[x1 + 1] - P[x0]) / (x1 - x0 + 1); }
  }
  std::vector<float> acc(w, 0.f);
  for (int y = 0; y <= std::min(r, h - 1); ++y) { const float* a = tmp.row(y); for (int x = 0; x < w; ++x) acc[x] += a[x]; }
  for (int y = 0; y < h; ++y) {
    int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
    float inv = 1.f / (y1 - y0 + 1); float* o = out.row(y);
    for (int x = 0; x < w; ++x) o[x] = acc[x] * inv;
    if (y + r + 1 < h) { const float* a = tmp.row(y + r + 1); for (int x = 0; x < w; ++x) acc[x] += a[x]; }
    if (y - r >= 0) { const float* a = tmp.row(y - r); for (int x = 0; x < w; ++x) acc[x] -= a[x]; }
  }
}

// He 2010. 가이드 I(휘도)의 엣지에 마스크 p를 붙인다. 박스 필터 6회.
void guided_filter(const Image<float>& I, const Image<float>& p, int r, float eps, Arena& s, Image<float>& q) {
  const int w = I.w, h = I.h;
  Image<float> tmp = s.alloc<float>(w, h), mI = s.alloc<float>(w, h), mp = s.alloc<float>(w, h);
  Image<float> II = s.alloc<float>(w, h), Ip = s.alloc<float>(w, h), a = s.alloc<float>(w, h), b = s.alloc<float>(w, h);
  for (int y = 0; y < h; ++y) {
    const float* i0 = I.row(y); const float* p0 = p.row(y); float* ii = II.row(y); float* ip = Ip.row(y);
    for (int x = 0; x < w; ++x) { ii[x] = i0[x] * i0[x]; ip[x] = i0[x] * p0[x]; }
  }
  box_filter(I, r, tmp, mI); box_filter(p, r, tmp, mp);
  box_filter(II, r, tmp, II); box_filter(Ip, r, tmp, Ip);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float mi = mI.at(x, y), var = II.at(x, y) - mi * mi, cov = Ip.at(x, y) - mi * mp.at(x, y);
      float av = cov / (var + eps);
      a.at(x, y) = av; b.at(x, y) = mp.at(x, y) - av * mi;
    }
  box_filter(a, r, tmp, a); box_filter(b, r, tmp, b);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) q.at(x, y) = a.at(x, y) * I.at(x, y) + b.at(x, y);
}

void resize_bilinear(const Image<float>& in, Image<float>& out) {
  const float sx = (float)in.w / out.w, sy = (float)in.h / out.h;
  for (int y = 0; y < out.h; ++y) {
    float fy = std::max(0.f, (y + 0.5f) * sy - 0.5f); int y0 = std::min((int)fy, in.h - 1), y1 = std::min(y0 + 1, in.h - 1); float wy = fy - y0;
    float* o = out.row(y);
    for (int x = 0; x < out.w; ++x) {
      float fx = std::max(0.f, (x + 0.5f) * sx - 0.5f); int x0 = std::min((int)fx, in.w - 1), x1 = std::min(x0 + 1, in.w - 1); float wx = fx - x0;
      o[x] = (1 - wy) * ((1 - wx) * in.at(x0, y0) + wx * in.at(x1, y0)) + wy * ((1 - wx) * in.at(x0, y1) + wx * in.at(x1, y1));
    }
  }
}

void luma_of(const Image<float>& rgb, float gain, Image<float>& y) {
  const int w = rgb.w / 3;
  for (int j = 0; j < rgb.h; ++j) {
    const float* s = rgb.row(j); float* o = y.row(j);
    for (int x = 0; x < w; ++x) o[x] = std::min(1.f, gain * (0.2126f * s[3 * x] + 0.7152f * s[3 * x + 1] + 0.0722f * s[3 * x + 2]));
  }
}

// 블러 전에 밝은 픽셀을 키워 보케 원이 살아나게 (클리핑된 광원은 선형값이 실제보다 작다)
void highlight_boost(Image<float>& rgb, float gain, float strength) {
  const int w = rgb.w / 3;
  for (int j = 0; j < rgb.h; ++j) {
    float* s = rgb.row(j);
    for (int x = 0; x < w; ++x) {
      float L = gain * (0.2126f * s[3 * x] + 0.7152f * s[3 * x + 1] + 0.0722f * s[3 * x + 2]);
      float f = std::min(1.f, std::max(0.f, (L - 0.7f) / 0.6f));
      float k = 1.f + strength * f * f;
      s[3 * x] *= k; s[3 * x + 1] *= k; s[3 * x + 2] *= k;
    }
  }
}

// 원형 커널(렌즈 보케) + (1−α) 가중 정규화: 전경색이 배경 블러로 번지는 헤일로를 막는다.
void disc_blur_normalized(const Image<float>& rgb, const Image<float>& alpha, int radius, ThreadPool& pool, Arena& scratch,
                          Image<float>& out) {
  const int w = rgb.w / 3, h = rgb.h, W1 = w + 1;
  // 1) 행별 누적합 P[y][x] = Σ_{x'<x} (w·r, w·g, w·b, w),  w = 1−α
  Image<float> P = scratch.alloc<float>(W1 * 4, h);
  pool.parallel_for(h, [&](int y) {
    const float* s = rgb.row(y); const float* a = alpha.row(y); float* p = P.row(y);
    float cr = 0, cg = 0, cb = 0, cw = 0;
    p[0] = p[1] = p[2] = p[3] = 0;
    for (int x = 0; x < w; ++x) {
      const float wt = 1.f - a[x];
      cr += wt * s[3 * x]; cg += wt * s[3 * x + 1]; cb += wt * s[3 * x + 2]; cw += wt;
      float* q = p + 4 * (x + 1); q[0] = cr; q[1] = cg; q[2] = cb; q[3] = cw;
    }
  });
  std::vector<int> span(2 * radius + 1);
  for (int dy = -radius; dy <= radius; ++dy) span[dy + radius] = (int)std::floor(std::sqrt((float)(radius * radius - dy * dy)));
  // 2) 픽셀마다 원 = 행 구간 [x−sx, x+sx] 합. 이미지 밖 열은 가장자리 값 복제 (직접 합산 구현과 같은 규칙)
  pool.parallel_for(h, [&](int y) {
    float* o = out.row(y);
    const float* c0 = rgb.row(y);
    for (int x = 0; x < w; ++x) {
      const float* c = c0 + 3 * x;
      if (alpha.at(x, y) > 0.98f) { o[3 * x] = c[0]; o[3 * x + 1] = c[1]; o[3 * x + 2] = c[2]; continue; }
      float sr = 0, sg = 0, sb = 0, sw = 0;
      for (int dy = -radius; dy <= radius; ++dy) {
        const int yy = std::min(h - 1, std::max(0, y + dy));
        const int sx = span[dy + radius];
        const float* p = P.row(yy);
        const int xa = std::max(0, x - sx), xb = std::min(w - 1, x + sx);
        const float* pa = p + 4 * xa; const float* pb = p + 4 * (xb + 1);
        sr += pb[0] - pa[0]; sg += pb[1] - pa[1]; sb += pb[2] - pa[2]; sw += pb[3] - pa[3];
        const int nl = xa - (x - sx), nr = (x + sx) - xb;  // 복제되는 바깥 탭 수
        if (nl > 0) {
          const float* e = p + 4; sr += nl * e[0]; sg += nl * e[1]; sb += nl * e[2]; sw += nl * e[3];
        }
        if (nr > 0) {
          const float* e1 = p + 4 * w; const float* e0 = p + 4 * (w - 1);
          sr += nr * (e1[0] - e0[0]); sg += nr * (e1[1] - e0[1]); sb += nr * (e1[2] - e0[2]); sw += nr * (e1[3] - e0[3]);
        }
      }
      if (sw < 1e-3f) { o[3 * x] = c[0]; o[3 * x + 1] = c[1]; o[3 * x + 2] = c[2]; }
      else { float inv = 1.f / sw; o[3 * x] = sr * inv; o[3 * x + 1] = sg * inv; o[3 * x + 2] = sb * inv; }
    }
  });
}

void disc_blur_direct(const Image<float>& rgb, const Image<float>& alpha, int radius, ThreadPool& pool, Image<float>& out) {
  // 행별 [dx0, dx1] 스팬으로 표현 → 내부 픽셀은 행 포인터 + 연속 접근
  std::vector<int> span(2 * radius + 1);
  for (int dy = -radius; dy <= radius; ++dy) span[dy + radius] = (int)std::floor(std::sqrt((float)(radius * radius - dy * dy)));
  const int w = rgb.w / 3, h = rgb.h;
  pool.parallel_for(h, [&](int y) {
    float* o = out.row(y);
    for (int x = 0; x < w; ++x) {
      const float* c = rgb.row(y) + 3 * x;
      if (alpha.at(x, y) > 0.98f) {  // L2: 전경은 composite에서 어차피 α·sharp. 블러 계산 생략
        o[3 * x] = c[0]; o[3 * x + 1] = c[1]; o[3 * x + 2] = c[2]; continue;
      }
      float sr = 0, sg = 0, sb = 0, sw = 0;
      const bool interior = x >= radius && x + radius < w && y >= radius && y + radius < h;
      for (int dy = -radius; dy <= radius; ++dy) {
        const int sx = span[dy + radius];
        const int yy = interior ? y + dy : std::min(h - 1, std::max(0, y + dy));
        const float* srow = rgb.row(yy); const float* arow = alpha.row(yy);
        if (interior) {
          for (int xx = x - sx; xx <= x + sx; ++xx) {
            const float wgt = 1.f - arow[xx]; const float* s = srow + 3 * xx;
            sr += wgt * s[0]; sg += wgt * s[1]; sb += wgt * s[2]; sw += wgt;
          }
        } else {
          for (int dx = -sx; dx <= sx; ++dx) {
            const int xx = std::min(w - 1, std::max(0, x + dx));
            const float wgt = 1.f - arow[xx]; const float* s = srow + 3 * xx;
            sr += wgt * s[0]; sg += wgt * s[1]; sb += wgt * s[2]; sw += wgt;
          }
        }
      }
      if (sw < 1e-3f) { o[3 * x] = c[0]; o[3 * x + 1] = c[1]; o[3 * x + 2] = c[2]; }
      else { float inv = 1.f / sw; o[3 * x] = sr * inv; o[3 * x + 1] = sg * inv; o[3 * x + 2] = sb * inv; }
    }
  });
}

// 출력 행마다: 1/4 해상도 α·블러의 두 행을 세로 보간해 행 버퍼(QW)로 만든 뒤, 열 테이블로 가로 보간.
// (픽셀마다 좌표·가중치를 다시 계산하고 채널마다 std::lround를 부르던 버전: C55에서 72–80 ms)
void composite(const Image<uint8_t>& sharp, const Image<uint8_t>& blur_q, const Image<float>& alpha_q, ThreadPool& pool, Image<uint8_t>& out) {
  const int W = sharp.w / 4, H = sharp.h, QW = alpha_q.w, QH = alpha_q.h;
  const float sx = (float)QW / W, sy = (float)QH / H;
  std::vector<int> cx0(W), cx1(W);
  std::vector<float> cwx(W);
  for (int x = 0; x < W; ++x) {
    const float fx = std::max(0.f, (x + 0.5f) * sx - 0.5f);
    cx0[x] = std::min((int)fx, QW - 1); cx1[x] = std::min(cx0[x] + 1, QW - 1); cwx[x] = fx - cx0[x];
  }
  pool.parallel_for(H, [&](int y) {
    thread_local std::vector<float> ra, rb;  // 세로 보간된 α 행, 블러 행(RGB)
    if ((int)ra.size() < QW) { ra.resize(QW); rb.resize((size_t)QW * 3); }
    const float fy = std::max(0.f, (y + 0.5f) * sy - 0.5f);
    const int y0 = std::min((int)fy, QH - 1), y1 = std::min(y0 + 1, QH - 1);
    const float wy = fy - y0;
    const float* a0 = alpha_q.row(y0); const float* a1 = alpha_q.row(y1);
    const uint8_t* b0 = blur_q.row(y0); const uint8_t* b1 = blur_q.row(y1);
    for (int q = 0; q < QW; ++q) {
      ra[q] = a0[q] + wy * (a1[q] - a0[q]);
      for (int c = 0; c < 3; ++c) rb[3 * q + c] = b0[4 * q + c] + wy * ((float)b1[4 * q + c] - b0[4 * q + c]);
    }
    const uint8_t* s = sharp.row(y); uint8_t* o = out.row(y);
    for (int x = 0; x < W; ++x) {
      const int q0 = cx0[x], q1 = cx1[x]; const float wx = cwx[x];
      float a = ra[q0] + wx * (ra[q1] - ra[q0]);
      a = std::min(1.f, std::max(0.f, a));
      for (int c = 0; c < 3; ++c) {
        const float bl = rb[3 * q0 + c] + wx * (rb[3 * q1 + c] - rb[3 * q0 + c]);
        o[4 * x + c] = (uint8_t)(a * s[4 * x + c] + (1 - a) * bl + 0.5f);  // 값 ≥ 0 → +0.5 절삭 = 반올림
      }
      o[4 * x + 3] = 255;
    }
  });
}

}  // namespace bp
