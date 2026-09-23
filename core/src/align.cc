#include "burstpipe/align.h"
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace bp {

void bayer_to_gray(const Image<uint16_t>& b, Image<uint16_t>& g) {
  for (int y = 0; y < g.h; ++y) {
    const uint16_t* r0 = b.row(2 * y); const uint16_t* r1 = b.row(2 * y + 1); uint16_t* o = g.row(y);
    for (int x = 0; x < g.w; ++x) o[x] = (uint16_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
  }
}

void downsample2(const Image<uint16_t>& in, Image<uint16_t>& out) { bayer_to_gray(in, out); }  // 같은 2x2 박스

void build_pyramid(const Image<uint16_t>& gray, int nlevels, Arena& arena, Pyramid& out) {
  out.levels.clear();
  out.levels.push_back(gray);
  for (int l = 1; l < nlevels; ++l) {
    const Image<uint16_t> prev = out.levels.back();
    if (prev.w < 4 || prev.h < 4) break;
    Image<uint16_t> next = arena.alloc<uint16_t>(prev.w / 2, prev.h / 2);
    downsample2(prev, next);
    out.levels.push_back(next);
  }
}

// 선명도 = 수평 그래디언트 절대합 (레벨1에서, 비용 절감)
int select_reference(const std::vector<Pyramid>& pyrs, int consider) {
  int best = 0; double best_s = -1;
  int n = std::min<int>(consider, (int)pyrs.size());
  for (int i = 0; i < n; ++i) {
    const Image<uint16_t>& g = pyrs[i].levels[std::min<size_t>(1, pyrs[i].levels.size() - 1)];
    double s = 0;
    for (int y = 0; y < g.h; ++y) {
      const uint16_t* r = g.row(y);
      for (int x = 1; x < g.w; ++x) s += std::abs((int)r[x] - (int)r[x - 1]);
    }
    if (s > best_s) { best_s = s; best = i; }
  }
  return best;
}

uint32_t tile_sad_scalar(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile) {
  uint32_t s = 0;
  for (int y = 0; y < tile; ++y, a += sa, b += sb)
    for (int x = 0; x < tile; ++x) s += (uint32_t)std::abs((int)a[x] - (int)b[x]);
  return s;
}

#if !defined(__ARM_NEON)
uint32_t tile_sad_neon(const uint16_t* a, int sa, const uint16_t* b, int sb, int tile) { return tile_sad_scalar(a, sa, b, sb, tile); }
#endif

void align_frame(const Pyramid& ref, const Pyramid& alt, const AlignParams& p, ThreadPool& pool, MotionField& out) {
  const int L = (int)std::min(ref.levels.size(), alt.levels.size());
  MotionField prev;
  for (int l = L - 1; l >= 0; --l) {
    const Image<uint16_t>& R = ref.levels[l];
    const Image<uint16_t>& A = alt.levels[l];
    const int T = p.tile;
    MotionField cur;
    cur.resize(std::max(1, R.w / T), std::max(1, R.h / T), T);
    const int radius = (l == L - 1) ? p.search_coarse : p.search_fine;
    const bool coarsest = (l == L - 1);
    pool.parallel_for(cur.tiles_y, [&](int ty) {
      for (int tx = 0; tx < cur.tiles_x; ++tx) {
        int dx0 = 0, dy0 = 0;
        if (!coarsest) {
          int ptx = std::min(tx / 2, prev.tiles_x - 1), pty = std::min(ty / 2, prev.tiles_y - 1);
          dx0 = 2 * prev.dx[prev.idx(ptx, pty)]; dy0 = 2 * prev.dy[prev.idx(ptx, pty)];
        }
        const int x0 = tx * T, y0 = ty * T;
        const int tw = std::min(T, R.w - x0), th = std::min(T, R.h - y0);
        uint32_t best = UINT32_MAX; int bdx = dx0, bdy = dy0;
        if (tw == T && th == T) {
          for (int ddy = -radius; ddy <= radius; ++ddy)
            for (int ddx = -radius; ddx <= radius; ++ddx) {
              int ax = x0 + dx0 + ddx, ay = y0 + dy0 + ddy;
              if (ax < 0 || ay < 0 || ax + T > A.w || ay + T > A.h) continue;
              uint32_t s = tile_sad(R.row(y0) + x0, R.stride, A.row(ay) + ax, A.stride, T);
              // 동점이면 이전 단 예측(ddx=ddy=0)에 가까운 쪽 — 평탄 타일에서 모션이 튀지 않게
              if (s < best || (s == best && std::abs(ddx) + std::abs(ddy) < std::abs(bdx - dx0) + std::abs(bdy - dy0))) {
                best = s; bdx = dx0 + ddx; bdy = dy0 + ddy;
              }
            }
        }
        int i = cur.idx(tx, ty);
        cur.dx[i] = (int16_t)bdx; cur.dy[i] = (int16_t)bdy;
        if (l == 0 && p.subpixel && best != UINT32_MAX) {
          // SAD(L1) 곡면은 V자 → 등각 직선 맞춤 d = (S₋ − S₊) / (2·(max(S₋,S₊) − S₀)), 축별
          auto sad_at = [&](int dx, int dy) -> int64_t {
            int ax = x0 + dx, ay = y0 + dy;
            if (ax < 0 || ay < 0 || ax + T > A.w || ay + T > A.h) return -1;
            return tile_sad(R.row(y0) + x0, R.stride, A.row(ay) + ax, A.stride, T);
          };
          auto fit = [&](int64_t sm, int64_t s0, int64_t sp) -> float {
            if (sm < 0 || sp < 0) return 0.f;
            const int64_t den = 2 * (std::max(sm, sp) - s0);
            if (den <= 0) return 0.f;
            return std::min(0.5f, std::max(-0.5f, (float)(sm - sp) / (float)den));
          };
          cur.fx[i] = fit(sad_at(bdx - 1, bdy), best, sad_at(bdx + 1, bdy));
          cur.fy[i] = fit(sad_at(bdx, bdy - 1), best, sad_at(bdx, bdy + 1));
        }
        cur.err[i] = best == UINT32_MAX ? 1e9f : (float)best / (float)(T * T);
      }
    });
    prev = std::move(cur);
  }
  out = std::move(prev);
}

}  // namespace bp
