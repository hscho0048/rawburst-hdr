// 주파수 영역 강건 합성 (Hasinoff et al. 2016 §5, HDR+). merge_burst()와 같은 입출력 규약.
//  - 합성 타일 T×T raw (stride T/2) → Bayer 색 평면 4개 (T/2)² 각각 2D DFT
//  - 주파수마다 D = T0 − Tz, Az = |D|² / (|D|² + c·σf²) 로 참조 쪽으로 수축: 노이즈 수준 차이는 평균, 큰 차이(움직임)는 참조
//  - 병합 T̃ = (1/N) Σz [Tz + Az (T0 − Tz)] → 역 DFT → raised-cosine 창으로 겹쳐 더함
// 정렬은 공간 합성과 같다 (겹치는 정렬 타일 모션 후보 중 footprint SAD 최소, gray→Bayer 짝수 이동).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "burstpipe/merge.h"

namespace bp {
namespace {

// n×n 2D FFT, 실수부/허수부 분리 배열.
//  - std::complex 곱셈은 -ffast-math 없이 NaN/Inf 처리 때문에 __mulsc3 호출이 된다 (C55: 합성 5.4 s) → 직접 전개.
//  - 열 방향 패스: 나비 한 쌍(행 p0, p1)을 모든 열에 대해 연속 메모리로 처리 → 컴파일러 NEON 자동 벡터화 (2.7 s → 아래 측정).
//  - 행 방향 = 전치 후 같은 열 패스. 순방향 결과는 "전치된 스펙트럼"으로 둔다: 주파수별 수축은 위치와 무관하고,
//    역방향도 같은 순서(열 패스 → 전치 → 열 패스)로 하면 원래 방향으로 돌아온다.
struct Fft2 {
  int n = 0, lg = 0;
  std::vector<float> twr, twi;
  std::vector<int> rev;
  explicit Fft2(int n_) : n(n_) {
    while ((1 << lg) < n) ++lg;
    twr.resize(n / 2); twi.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) { twr[k] = std::cos(2.f * 3.14159265f * k / n); twi[k] = -std::sin(2.f * 3.14159265f * k / n); }
    rev.resize(n);
    for (int i = 0; i < n; ++i) { int r = 0; for (int bb = 0; bb < lg; ++bb) if (i & (1 << bb)) r |= 1 << (lg - 1 - bb); rev[i] = r; }
  }
  // 모든 열을 동시에 세로 FFT
  void cols(float* __restrict re, float* __restrict im, bool inverse) const {
    for (int i = 0; i < n; ++i)
      if (i < rev[i])
        for (int c = 0; c < n; ++c) { std::swap(re[i * n + c], re[rev[i] * n + c]); std::swap(im[i * n + c], im[rev[i] * n + c]); }
    const float sgn = inverse ? -1.f : 1.f;
    for (int len = 2; len <= n; len <<= 1) {
      const int step = n / len, h = len / 2;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < h; ++j) {
          const float wr = twr[j * step], wi = sgn * twi[j * step];
          float* __restrict r0 = re + (i + j) * n; float* __restrict i0 = im + (i + j) * n;
          float* __restrict r1 = re + (i + j + h) * n; float* __restrict i1 = im + (i + j + h) * n;
          for (int c = 0; c < n; ++c) {
            const float vr = r1[c] * wr - i1[c] * wi, vi = r1[c] * wi + i1[c] * wr;
            const float ur = r0[c], ui = i0[c];
            r0[c] = ur + vr; i0[c] = ui + vi; r1[c] = ur - vr; i1[c] = ui - vi;
          }
        }
    }
  }
  void transpose(float* a, float* tmp) const {
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) tmp[x * n + y] = a[y * n + x];
    std::copy(tmp, tmp + n * n, a);
  }
  void run2d(float* re, float* im, bool inverse, float* tmp) const {
    cols(re, im, inverse); transpose(re, tmp); transpose(im, tmp); cols(re, im, inverse);
  }
};

// 실수 평면 두 개(a, b)를 re/im에 넣고 FFT 한 번 → 대칭으로 분리 (FFT 수 절반).
//   A[k] = ½(X[k] + conj X[−k]),  B[k] = ½((Xi[k]+Xi[−k]) − i(Xr[k]−Xr[−k]))
void unpack_pair(const float* xr, const float* xi, int n, float* ar, float* ai, float* br, float* bi) {
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) {
      const int k = r * n + c, nk = ((n - r) % n) * n + (n - c) % n;
      ar[k] = 0.5f * (xr[k] + xr[nk]); ai[k] = 0.5f * (xi[k] - xi[nk]);
      br[k] = 0.5f * (xi[k] + xi[nk]); bi[k] = -0.5f * (xr[k] - xr[nk]);
    }
}

}  // namespace

MergeStats merge_burst_wiener(const Burst& b, int ref, const std::vector<MotionField>& fields, const MergeParams& p,
                              ThreadPool& pool, Arena& scratch, Image<uint16_t>& merged, std::vector<float>* weights) {
  const int W = b.meta.width, H = b.meta.height, T = p.tile, S = T / 2, n = T / 2;  // n: 평면 타일 크기
  const int N = (int)b.frames.size();
  Image<float> num = scratch.alloc<float>(W, H), den = scratch.alloc<float>(W, H);
  pool.parallel_for(H, [&](int y) { std::fill(num.row(y), num.row(y) + W, 0.f); std::fill(den.row(y), den.row(y) + W, 0.f); });
  std::vector<float> win(T);
  for (int i = 0; i < T; ++i) win[i] = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (i + 0.5f) / T);
  const bool have_profile = noise_profile_plausible(b.meta);
  const float fallback_ed = have_profile ? 0.f : expected_diff_from_fields(fields, ref);
  const float black = 0.25f * (b.meta.black_level[0] + b.meta.black_level[1] + b.meta.black_level[2] + b.meta.black_level[3]);
  const float range = std::max(1.f, (float)b.meta.white_level - black);
  const MotionField& F0 = fields[ref == 0 ? 1 : 0];
  const Image<uint16_t>& R = b.frames[ref];
  const int mtx = (W + S - 1) / S, mty = (H + S - 1) / S;
  std::vector<double> wsum_per_row(mty, 0.0);
  std::vector<int> wcnt_per_row(mty, 0);
  if (weights) weights->assign((size_t)mtx * mty, 0.f);
  const Fft2 fft(n);
  const float c = p.wiener_c;

  for (int parity = 0; parity < 2; ++parity) {
    pool.parallel_for((mty + 1 - parity) / 2, [&](int j) {
      const int my = 2 * j + parity;
      const int y0 = my * S;
      const size_t nn = (size_t)n * n;
      std::vector<float> t0r(4 * nn), t0i(4 * nn), tzr(4 * nn), tzi(4 * nn), acr(4 * nn), aci(4 * nn), tmp(nn),
                         pr(nn), pi(nn);
      // 평면 pl = (행 위상 pl>>1, 열 위상 pl&1). 쌍 (0,1), (2,3)을 한 FFT로
      auto fwd_pairs = [&](const Image<uint16_t>& img, int ox, int oy, float* outr, float* outi) {
        for (int q = 0; q < 2; ++q) {
          for (int yy = 0; yy < n; ++yy) {
            const uint16_t* rr = img.row(oy + 2 * yy + q) + ox;
            for (int xx = 0; xx < n; ++xx) { pr[yy * n + xx] = rr[2 * xx]; pi[yy * n + xx] = rr[2 * xx + 1]; }
          }
          fft.run2d(pr.data(), pi.data(), false, tmp.data());
          const int pa = 2 * q, pb = 2 * q + 1;
          unpack_pair(pr.data(), pi.data(), n, outr + pa * nn, outi + pa * nn, outr + pb * nn, outi + pb * nn);
        }
      };
      std::vector<int> bdx(N), bdy(N);
      std::vector<char> use(N);
      for (int mx = 0; mx < mtx; ++mx) {
        const int x0 = mx * S;
        if (x0 + T > W || y0 + T > H) {  // 가장자리 부분 타일: 참조 그대로 (몇 픽셀, 창 가중)
          const int tw = std::min(T, W - x0), th = std::min(T, H - y0);
          for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
              const float g = win[y] * win[x];
              num.at(x0 + x, y0 + y) += g * R.at(x0 + x, y0 + y); den.at(x0 + x, y0 + y) += g;
            }
          continue;
        }
        // 노이즈: 평면별 σ² = (a·x + b)·range², x = 타일 평균
        double s = 0;
        for (int y = 0; y < T; ++y) { const uint16_t* r = R.row(y0 + y) + x0; for (int x = 0; x < T; ++x) s += r[x]; }
        const float mean = (float)(s / (T * T));
        float sigma2;
        if (have_profile) {
          const float xn = std::max(0.f, (mean - black) / range);
          sigma2 = std::max(b.meta.noise_a * xn + b.meta.noise_b, 1e-12f) * range * range;
        } else {
          const float ed = fallback_ed;  // gray 평균 |diff| ≈ 0.564σ → σ ≈ ed/0.564
          sigma2 = (ed / 0.564f) * (ed / 0.564f);
        }
        const float sigma_f2 = (float)(n * n) * sigma2;  // 비정규화 DFT: 각 계수 분산 = n²σ²
        // 정렬 후보 (공간 합성과 동일 규칙, Bayer SAD)
        const int gx0 = std::min((x0 / 2) / F0.tile, F0.tiles_x - 1), gx1 = std::min(((x0 + T - 1) / 2) / F0.tile, F0.tiles_x - 1);
        const int gy0 = std::min((y0 / 2) / F0.tile, F0.tiles_y - 1), gy1 = std::min(((y0 + T - 1) / 2) / F0.tile, F0.tiles_y - 1);
        int nuse = 1;
        for (int i = 0; i < N; ++i) {
          use[i] = 0;
          if (i == ref) continue;
          const MotionField& f = fields[i];
          uint32_t best = UINT32_MAX;
          for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
              const int gi = f.idx(gx, gy);
              const int cdx = 2 * f.dx[gi], cdy = 2 * f.dy[gi];
              if (x0 + cdx < 0 || x0 + T + cdx > W || y0 + cdy < 0 || y0 + T + cdy > H) continue;
              const uint32_t sad = tile_sad(R.row(y0) + x0, R.stride, b.frames[i].row(y0 + cdy) + x0 + cdx, b.frames[i].stride, T);
              if (sad < best) { best = sad; bdx[i] = cdx; bdy[i] = cdy; }
            }
          if (best != UINT32_MAX) { use[i] = 1; ++nuse; }
        }
        // 참조 평면 DFT
        fwd_pairs(R, x0, y0, t0r.data(), t0i.data());
        std::copy(t0r.begin(), t0r.end(), acr.begin());  // z = ref 항: T0
        std::copy(t0i.begin(), t0i.end(), aci.begin());
        double wsum = 0;
        for (int i = 0; i < N; ++i) {
          if (!use[i]) continue;
          const Image<uint16_t>& A = b.frames[i];
          double shrink = 0;
          fwd_pairs(A, x0 + bdx[i], y0 + bdy[i], tzr.data(), tzi.data());
          for (int pl = 0; pl < 4; ++pl) {
            const float* zr = &tzr[pl * nn]; const float* zi = &tzi[pl * nn];
            const float* r0 = &t0r[pl * nn]; const float* i0 = &t0i[pl * nn];
            float* cr = &acr[pl * nn]; float* ci = &aci[pl * nn];
            const float cs = c * sigma_f2;
            for (size_t k = 0; k < nn; ++k) {
              const float dr = r0[k] - zr[k], di = i0[k] - zi[k];
              const float d2 = dr * dr + di * di;
              const float az = d2 / (d2 + cs);
              cr[k] += zr[k] + az * dr; ci[k] += zi[k] + az * di;
              shrink += az;
            }
          }
          const double wi = 1.0 - shrink / (4.0 * n * n);  // 이 프레임을 얼마나 받아들였나 (시각화·통계용)
          wsum += wi; wsum_per_row[my] += wi; wcnt_per_row[my] += 1;
        }
        if (weights) (*weights)[(size_t)my * mtx + mx] = N > 1 ? (float)(wsum / (N - 1)) : 0.f;
        // 역 DFT (1/n² 정규화, 1/nuse 평균) → 창 가중 누적
        const float inv = 1.f / ((float)(n * n) * nuse);
        for (int q = 0; q < 2; ++q) {
          const float* ar_ = &acr[(2 * q) * nn]; const float* ai_ = &aci[(2 * q) * nn];
          const float* br_ = &acr[(2 * q + 1) * nn]; const float* bi_ = &aci[(2 * q + 1) * nn];
          for (size_t k = 0; k < nn; ++k) { pr[k] = ar_[k] - bi_[k]; pi[k] = ai_[k] + br_[k]; }
          fft.run2d(pr.data(), pi.data(), true, tmp.data());
          for (int yy = 0; yy < n; ++yy) {
            const int ly = 2 * yy + q;
            float* nrow = num.row(y0 + ly) + x0; float* drow = den.row(y0 + ly) + x0;
            const float wy = win[ly];
            for (int xx = 0; xx < n; ++xx) {
              const float g0 = wy * win[2 * xx], g1 = wy * win[2 * xx + 1];
              nrow[2 * xx] += g0 * pr[yy * n + xx] * inv; drow[2 * xx] += g0;
              nrow[2 * xx + 1] += g1 * pi[yy * n + xx] * inv; drow[2 * xx + 1] += g1;
            }
          }
        }
      }
    });
  }
  pool.parallel_for(H, [&](int y) {
    const float* nr = num.row(y); const float* d = den.row(y); uint16_t* o = merged.row(y);
    for (int x = 0; x < W; ++x) { float v = nr[x] / d[x] + 0.5f; o[x] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
  });
  MergeStats st;
  double ws = 0; long wc = 0;
  for (int i = 0; i < mty; ++i) { ws += wsum_per_row[i]; wc += wcnt_per_row[i]; }
  st.mean_weight = wc ? (float)(ws / wc) : 0.f;
  st.expected_diff = have_profile ? -1.f : fallback_ed;
  return st;
}

}  // namespace bp
