#include "burstpipe/superres.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "burstpipe/merge.h"

namespace bp {

void merge_superres(const Burst& b, int ref, const std::vector<MotionField>& fields, const SrParams& p, ThreadPool& pool,
                    Image<float>& rgb) {
  const BurstMeta& m = b.meta;
  const int W = m.width, H = m.height, N = (int)b.frames.size();
  const MotionField& F0 = fields[ref == 0 ? 1 : 0];
  const int TX = F0.tiles_x, TY = F0.tiles_y, GT = F0.tile;  // gray 타일 → raw 2·GT
  const int RT = 2 * GT;
  const bool have_profile = noise_profile_plausible(m);
  const float fallback_ed = have_profile ? 0.f : expected_diff_from_fields(fields, ref);
  const Image<uint16_t>& R = b.frames[ref];

  // 1) 프레임×정렬 타일 강건성 가중치 (정수 이동 SAD vs 노이즈 기대치, 합성과 같은 규칙)
  std::vector<float> rob((size_t)N * TX * TY, 0.f);
  pool.parallel_for(TY, [&](int ty) {
    for (int tx = 0; tx < TX; ++tx) {
      const int x0 = tx * RT, y0 = ty * RT;
      if (x0 + RT > W || y0 + RT > H) { for (int i = 0; i < N; ++i) rob[((size_t)i * TY + ty) * TX + tx] = i == ref; continue; }
      float ed = fallback_ed;
      if (have_profile) {
        double s = 0;
        for (int y = 0; y < RT; ++y) { const uint16_t* r = R.row(y0 + y) + x0; for (int x = 0; x < RT; ++x) s += r[x]; }
        ed = expected_diff_from_profile(m, (float)(s / (RT * RT)));
      }
      const float ed_b = 2.f * ed;
      for (int i = 0; i < N; ++i) {
        float w = 0.f;
        if (i == ref) w = 1.f;
        else {
          const int gi = fields[i].idx(tx, ty);
          const int bx = 2 * fields[i].dx[gi], by = 2 * fields[i].dy[gi];
          if (x0 + bx >= 0 && x0 + RT + bx <= W && y0 + by >= 0 && y0 + RT + by <= H) {
            const float e = (float)tile_sad(R.row(y0) + x0, R.stride, b.frames[i].row(y0 + by) + x0 + bx, b.frames[i].stride, RT) / (RT * RT);
            w = std::min(1.f, std::max(0.f, (p.k * ed_b - e) / ((p.k - 1.f) * ed_b)));
          }
        }
        rob[((size_t)i * TY + ty) * TX + tx] = w;
      }
    }
  });

  // 1.5) 이방성 커널: ref gray(½ 해상도)의 구조 텐서 (3×3 박스 평균 그래디언트 곱) → 픽셀마다 고유벡터 e1(엣지 가로 방향),
  //      A = 엣지 강도 → σ_along = σ·(1 + (k_stretch−1)·A), σ_across = σ·(1 − (1−k_shrink)·A)
  const int GW = W / 2, GH = H / 2;
  std::vector<float> st_c, st_s, st_a;  // cosθ, sinθ (e1 = 그래디언트 방향), 강도 A∈[0,1]
  if (p.anisotropic) {
    std::vector<float> g((size_t)GW * GH);
    for (int y = 0; y < GH; ++y) {
      const uint16_t* r0 = R.row(2 * y); const uint16_t* r1 = R.row(2 * y + 1);
      for (int x = 0; x < GW; ++x) g[(size_t)y * GW + x] = 0.25f * (r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1]);
    }
    std::vector<float> ixx((size_t)GW * GH), iyy((size_t)GW * GH), ixy((size_t)GW * GH);
    for (int y = 1; y < GH - 1; ++y)
      for (int x = 1; x < GW - 1; ++x) {
        const size_t i = (size_t)y * GW + x;
        const float gx = 0.5f * (g[i + 1] - g[i - 1]), gy = 0.5f * (g[i + GW] - g[i - GW]);
        ixx[i] = gx * gx; iyy[i] = gy * gy; ixy[i] = gx * gy;
      }
    st_c.assign((size_t)GW * GH, 1.f); st_s.assign((size_t)GW * GH, 0.f); st_a.assign((size_t)GW * GH, 0.f);
    const float black = 0.25f * (m.black_level[0] + m.black_level[1] + m.black_level[2] + m.black_level[3]);
    const float norm = p.edge_d * (m.white_level - black);
    pool.parallel_for(GH, [&](int y) {
      if (y < 2 || y >= GH - 2) return;
      for (int x = 2; x < GW - 2; ++x) {
        float a = 0, c = 0, d = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) { const size_t i = (size_t)(y + dy) * GW + x + dx; a += ixx[i]; c += iyy[i]; d += ixy[i]; }
        a /= 9; c /= 9; d /= 9;
        const float tr = a + c, det = a * c - d * d, disc = std::sqrt(std::max(0.f, tr * tr / 4 - det));
        const float l1 = tr / 2 + disc, l2 = std::max(0.f, tr / 2 - disc);
        const float th = 0.5f * std::atan2(2 * d, a - c);  // 주 고유벡터(그래디언트) 각도
        const size_t i = (size_t)y * GW + x;
        st_c[i] = std::cos(th); st_s[i] = std::sin(th);
        const float strength = std::sqrt(std::max(0.f, l1 - l2)) / norm;   // 방향성 있는 그래디언트 크기
        st_a[i] = std::min(1.f, strength);
      }
    });
  }

  // 2) 가우시안 LUT (d² → exp(−d²/2σ²)), 채널별 σ
  const int LUT = 1024;
  const float dmax2 = 2.f * (p.radius + 1) * (p.radius + 1);
  std::vector<float> lut_g(LUT + 1), lut_rb(LUT + 1);
  for (int i = 0; i <= LUT; ++i) {
    const float d2 = dmax2 * i / LUT;
    lut_g[i] = std::exp(-d2 / (2 * p.sigma_g * p.sigma_g));
    lut_rb[i] = std::exp(-d2 / (2 * p.sigma_rb * p.sigma_rb));
  }
  const float lut_scale = LUT / dmax2;
  const int Rr = p.radius;

  // 3) 출력 픽셀마다 모든 프레임 샘플 누적
  pool.parallel_for(H, [&](int y) {
    float* o = rgb.row(y);
    const int ty = std::min(y / RT, TY - 1);
    for (int x = 0; x < W; ++x) {
      const int tx = std::min(x / RT, TX - 1);
      float num[3] = {0, 0, 0}, den[3] = {0, 0, 0};
      // 이 픽셀의 커널 축: e1 = 그래디언트 방향(엣지 가로질러), e2 = 엣지 방향. 거리를 σ 비율로 재스케일한 뒤 등방 LUT 사용
      float ec = 1.f, es = 0.f, s1 = 1.f, s2 = 1.f;
      if (p.anisotropic) {
        const size_t gi = (size_t)std::min(y / 2, GH - 1) * GW + std::min(x / 2, GW - 1);
        ec = st_c[gi]; es = st_s[gi];
        const float A = st_a[gi];
        s1 = 1.f / (1.f - (1.f - p.k_shrink) * A);   // 가로 방향: σ 줄임 → 거리 늘림
        s2 = 1.f / (1.f + (p.k_stretch - 1.f) * A);  // 엣지 방향: σ 늘림 → 거리 줄임
      }
      for (int i = 0; i < N; ++i) {
        const float w = rob[((size_t)i * TY + ty) * TX + tx];
        if (w <= 0.f) continue;
        float mx = 0, my = 0;
        if (i != ref) {
          const int gi = fields[i].idx(tx, ty);
          mx = 2.f * (fields[i].dx[gi] + (fields[i].fx.empty() ? 0.f : fields[i].fx[gi]));
          my = 2.f * (fields[i].dy[gi] + (fields[i].fy.empty() ? 0.f : fields[i].fy[gi]));
        }
        const float cx = x + mx, cy = y + my;  // 프레임 i 좌표 (alt(x+m) ≈ ref(x))
        const int ux = (int)std::lround(cx), uy = (int)std::lround(cy);
        const Image<uint16_t>& F = b.frames[i];
        for (int v = uy - Rr; v <= uy + Rr; ++v) {
          if (v < 0 || v >= H) continue;
          const uint16_t* row = F.row(v);
          const float dy = v - cy;
          for (int u = ux - Rr; u <= ux + Rr; ++u) {
            if (u < 0 || u >= W) continue;
            const float dx = u - cx;
            const float a1 = (dx * ec + dy * es) * s1, a2 = (-dx * es + dy * ec) * s2;
            const float d2 = a1 * a1 + a2 * a2;
            if (d2 >= dmax2) continue;
            const int c = kCfaColor[m.cfa][(v & 1) * 2 + (u & 1)];
            const int ch = c == 0 ? 0 : c == 3 ? 2 : 1;
            const float k = w * (ch == 1 ? lut_g : lut_rb)[(int)(d2 * lut_scale)];
            const float bl = m.black_at(u, v);
            const float val = std::min(1.f, std::max(0.f, (row[u] - bl) * m.gain_at(u, v) / (m.white_level - bl)));
            num[ch] += k * val; den[ch] += k;
          }
        }
      }
      for (int c = 0; c < 3; ++c) o[3 * x + c] = den[c] > 1e-6f ? num[c] / den[c] : 0.f;
    }
  });
}

}  // namespace bp
