#include "burstpipe/merge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace bp {

// 가중치 공식 (설계문서 5.3): 합성 타일 footprint의 평균 |ref−alt| e(Bayer raw 단위), 노이즈만으로 기대되는 값 ed_b일 때
//   w = clamp((k·ed_b − e) / ((k−1)·ed_b), 0, 1).   e ≤ ed_b → 1, e ≥ k·ed_b → 0.
// expected_diff_*는 gray 단위 ed = 0.564·σ·range (σ² = a·x + b, x = (tile_mean − black)/range).
//   (gray는 4픽셀 평균 → 분산/4, 두 프레임 차 → ×2 → σ²/2, |N(0,v)| 평균 = 0.798√v → 0.564σ)
//   Bayer 1픽셀 차는 분산 2σ² → 1.128σ = 2·ed.
float expected_diff_from_profile(const BurstMeta& m, float mean_raw) {
  float black = (m.black_level[0] + m.black_level[1] + m.black_level[2] + m.black_level[3]) * 0.25f;
  float range = std::max(1.f, (float)m.white_level - black);
  float x = std::max(0.f, (mean_raw - black) / range);
  float sigma2 = std::max(m.noise_a * x + m.noise_b, 1e-12f);
  return 0.564f * std::sqrt(sigma2) * range;
}

bool noise_profile_plausible(const BurstMeta& m) {
  return (m.noise_a > 0 || m.noise_b > 0) && m.noise_a <= 0.05f && m.noise_b <= 0.01f && m.noise_a >= 0 && m.noise_b >= 0;
}

// 노이즈 프로파일이 없을 때: 정적 타일이 다수라는 가정으로 err 중앙값을 기대 오차로 쓴다
float expected_diff_from_fields(const std::vector<MotionField>& fields, int ref) {
  std::vector<float> v;
  for (size_t i = 0; i < fields.size(); ++i) {
    if ((int)i == ref) continue;
    for (float e : fields[i].err) if (e < 1e8f) v.push_back(e);
  }
  if (v.empty()) return 1.f;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return std::max(v[v.size() / 2], 1e-3f);
}

void merge_row_scalar(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                      float win_y, int tw, float* num, float* den) {
  for (int x = 0; x < tw; ++x) {
    float acc = ref[x], ws = 1.f;
    for (int i = 0; i < nalt; ++i) {
      if (!alts[i]) continue;
      acc += w[i] * alts[i][x]; ws += w[i];
    }
    float g = win_y * win_x[x];
    num[x] += g * acc; den[x] += g * ws;
  }
}

#if !defined(__ARM_NEON)
void merge_row_neon(const uint16_t* ref, const uint16_t* const* alts, const float* w, int nalt, const float* win_x,
                    float win_y, int tw, float* num, float* den) {
  merge_row_scalar(ref, alts, w, nalt, win_x, win_y, tw, num, den);
}
#endif

// 타일 우선 루프: 겹침 타일(T, stride T/2) 하나에 대해 N프레임을 한 번에 누적한다 (L2 패스 융합).
// 프레임 우선으로 돌면 프레임마다 전체 이미지를 다시 읽어 캐시를 버린다.
MergeStats merge_burst(const Burst& b, int ref, const std::vector<MotionField>& fields, const MergeParams& p,
                       ThreadPool& pool, Arena& scratch, Image<uint16_t>& merged, std::vector<float>* weights) {
  const int W = b.meta.width, H = b.meta.height, T = p.tile, S = T / 2;
  const int N = (int)b.frames.size();
  Image<float> num = scratch.alloc<float>(W, H), den = scratch.alloc<float>(W, H);
  pool.parallel_for(H, [&](int y) { std::fill(num.row(y), num.row(y) + W, 0.f); std::fill(den.row(y), den.row(y) + W, 0.f); });

  std::vector<float> win(T);
  for (int i = 0; i < T; ++i) win[i] = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (i + 0.5f) / T);

  // HAL이 엉터리 프로파일을 주는 경우가 있다 (Android 에뮬레이터: a=1.0 → 모든 타일이 "노이즈 이내"라 고스트 거부가 꺼짐).
  // 정규화 raw 기준 a ≤ 0.05 (ISO ~3만 상당), b ≤ 0.01 을 넘으면 없는 것으로 보고 정렬 오차 중앙값으로 추정한다.
  const bool have_profile = noise_profile_plausible(b.meta);
  const float fallback_ed = have_profile ? 0.f : expected_diff_from_fields(fields, ref);
  const int any = ref == 0 ? 1 : 0;  // tiles_x 등을 읽을 비-ref 필드
  const MotionField& F0 = fields[any];

  const int mtx = (W + S - 1) / S, mty = (H + S - 1) / S;
  std::vector<double> wsum_per_row(mty, 0.0);
  std::vector<int> wcnt_per_row(mty, 0);
  if (weights) weights->assign((size_t)mtx * mty, 0.f);
  const Image<uint16_t>& R = b.frames[ref];

  for (int parity = 0; parity < 2; ++parity) {  // 겹치는 행을 동시에 쓰지 않도록 짝/홀 분리
    pool.parallel_for((mty + 1 - parity) / 2, [&](int j) {
      const int my = 2 * j + parity;
      const int y0 = my * S, th = std::min(T, H - y0);
      std::vector<const uint16_t*> rows(N, nullptr);
      std::vector<float> w(N, 0.f);
      std::vector<int> bdx(N, 0), bdy(N, 0);
      for (int mx = 0; mx < mtx; ++mx) {
        const int x0 = mx * S, tw = std::min(T, W - x0);
        // 타일 평균 (노이즈 모델용)
        float ed = fallback_ed;
        if (have_profile) {
          double s = 0;
          for (int y = 0; y < th; ++y) { const uint16_t* r = R.row(y0 + y) + x0; for (int x = 0; x < tw; ++x) s += r[x]; }
          ed = expected_diff_from_profile(b.meta, (float)(s / (tw * th)));
        }
        // 합성 타일(Bayer T×T)은 gray 정렬 타일 경계에 걸친다 → 겹치는 정렬 타일들(최대 2×2)의 모션을 후보로,
        // 합성 타일 footprint 전체의 Bayer 평균 |ref−alt|가 가장 작은 것을 고르고, 그 값으로 가중치를 정한다.
        // (정렬 타일 하나의 err만 쓰면, 평탄한 조명 내부의 엉뚱한 모션이 옆 타일의 엣지에 적용돼 밝기가 번진다.)
        const float ed_b = 2.f * ed;  // gray(4px 평균) → Bayer 1px: σ×2
        const int gx0 = std::min((x0 / 2) / F0.tile, F0.tiles_x - 1), gx1 = std::min(((x0 + tw - 1) / 2) / F0.tile, F0.tiles_x - 1);
        const int gy0 = std::min((y0 / 2) / F0.tile, F0.tiles_y - 1), gy1 = std::min(((y0 + th - 1) / 2) / F0.tile, F0.tiles_y - 1);
        float tile_wsum = 0;
        for (int i = 0; i < N; ++i) {
          if (i == ref) { w[i] = 0; continue; }
          const MotionField& f = fields[i];
          const Image<uint16_t>& A = b.frames[i];
          uint32_t best = UINT32_MAX;
          int cand_seen[4][2], nseen = 0;
          for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
              const int gi = f.idx(gx, gy);
              const int cdx = 2 * f.dx[gi], cdy = 2 * f.dy[gi];  // gray → Bayer: 짝수 이동이라 CFA 위상 유지
              bool dup = false;
              for (int s = 0; s < nseen; ++s) dup |= cand_seen[s][0] == cdx && cand_seen[s][1] == cdy;
              if (dup) continue;
              cand_seen[nseen][0] = cdx; cand_seen[nseen][1] = cdy; ++nseen;
              if (x0 + cdx < 0 || x0 + tw + cdx > W || y0 + cdy < 0 || y0 + th + cdy > H) continue;
              const uint16_t* rp = R.row(y0) + x0; const uint16_t* ap = A.row(y0 + cdy) + x0 + cdx;
              uint32_t sad;
              if (tw == T && th == T && (T % 8) == 0) {
                sad = tile_sad(rp, R.stride, ap, A.stride, T);
              } else {
                sad = 0;
                for (int y = 0; y < th; ++y, rp += R.stride, ap += A.stride)
                  for (int x = 0; x < tw; ++x) sad += (uint32_t)std::abs((int)rp[x] - (int)ap[x]);
              }
              if (sad < best) { best = sad; bdx[i] = cdx; bdy[i] = cdy; }
            }
          float wi = 0.f;
          if (best != UINT32_MAX) {  // 후보가 모두 이미지 밖이면 이 타일에서 제외
            const float e = (float)best / (float)(tw * th);
            wi = std::min(1.f, std::max(0.f, (p.k * ed_b - e) / ((p.k - 1.f) * ed_b)));
          }
          w[i] = wi;
          tile_wsum += wi;
          wsum_per_row[my] += wi; wcnt_per_row[my] += 1;
        }
        if (weights) (*weights)[(size_t)my * mtx + mx] = N > 1 ? tile_wsum / (N - 1) : 0.f;
        // ref를 제외한 가중치 배열 (타일당 1회)
        float wc[kMaxFrames]; int nalt = 0;
        for (int i = 0; i < N; ++i) if (i != ref) wc[nalt++] = w[i];
        for (int y = 0; y < th; ++y) {
          const int py = y0 + y;
          int k = 0;
          for (int i = 0; i < N; ++i) {
            if (i == ref) continue;
            const int sy = py + bdy[i];
            rows[k] = (w[i] > 0 && sy >= 0 && sy < H) ? b.frames[i].row(sy) + x0 + bdx[i] : nullptr;
            ++k;
          }
          merge_row(R.row(py) + x0, rows.data(), wc, nalt, win.data(), win[y], tw, num.row(py) + x0, den.row(py) + x0);
        }
      }
    });
  }
  pool.parallel_for(H, [&](int y) {
    const float* n = num.row(y); const float* d = den.row(y); uint16_t* o = merged.row(y);
    for (int x = 0; x < W; ++x) { float v = n[x] / d[x] + 0.5f; o[x] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
  });
  MergeStats st;
  double ws = 0; long wc = 0;
  for (int i = 0; i < mty; ++i) { ws += wsum_per_row[i]; wc += wcnt_per_row[i]; }
  st.mean_weight = wc ? (float)(ws / wc) : 0.f;
  st.expected_diff = have_profile ? -1.f : fallback_ed;
  return st;
}

}  // namespace bp
