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
void disc_blur_normalized(const Image<float>& rgb, const Image<float>& alpha, int radius, ThreadPool& pool, Image<float>& out) {
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

void composite(const Image<uint8_t>& sharp, const Image<uint8_t>& blur_q, const Image<float>& alpha_q, ThreadPool& pool, Image<uint8_t>& out) {
  const int W = sharp.w / 4, H = sharp.h, QW = alpha_q.w, QH = alpha_q.h;
  const float sx = (float)QW / W, sy = (float)QH / H;
  pool.parallel_for(H, [&](int y) {
    float fy = std::max(0.f, (y + 0.5f) * sy - 0.5f); int y0 = std::min((int)fy, QH - 1), y1 = std::min(y0 + 1, QH - 1); float wy = fy - y0;
    const uint8_t* s = sharp.row(y); uint8_t* o = out.row(y);
    for (int x = 0; x < W; ++x) {
      float fx = std::max(0.f, (x + 0.5f) * sx - 0.5f); int x0 = std::min((int)fx, QW - 1), x1 = std::min(x0 + 1, QW - 1); float wx = fx - x0;
      float w00 = (1 - wy) * (1 - wx), w01 = (1 - wy) * wx, w10 = wy * (1 - wx), w11 = wy * wx;
      float a = w00 * alpha_q.at(x0, y0) + w01 * alpha_q.at(x1, y0) + w10 * alpha_q.at(x0, y1) + w11 * alpha_q.at(x1, y1);
      a = std::min(1.f, std::max(0.f, a));
      for (int c = 0; c < 3; ++c) {
        float bl = w00 * blur_q.at(4 * x0 + c, y0) + w01 * blur_q.at(4 * x1 + c, y0) + w10 * blur_q.at(4 * x0 + c, y1) + w11 * blur_q.at(4 * x1 + c, y1);
        o[4 * x + c] = (uint8_t)std::lround(a * s[4 * x + c] + (1 - a) * bl);
      }
      o[4 * x + 3] = 255;
    }
  });
}

}  // namespace bp
