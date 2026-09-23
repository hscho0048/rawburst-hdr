#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "burstpipe/merge.h"

int main() {
  const int W = 256, H = 192, N = 4;
  bp::Burst b;
  b.meta.width = W; b.meta.height = H; b.meta.white_level = 4095;
  b.meta.noise_a = 1e-4f; b.meta.noise_b = 1e-6f;
  for (int i = 0; i < N; ++i) b.meta.frames.push_back({});
  std::vector<bp::Buffer<uint16_t>> bufs(N);
  std::srand(7);
  for (int i = 0; i < N; ++i) bufs[i].resize(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      uint16_t v = (uint16_t)(200 + std::rand() % 3000);
      for (int i = 0; i < N; ++i) bufs[i].img.at(x, y) = v;
    }
  for (int i = 0; i < N; ++i) b.frames.push_back(bufs[i].img);

  // 모션 0, 오차 0 (완전 정적)
  std::vector<bp::MotionField> fields(N);
  for (int i = 1; i < N; ++i) fields[i].resize(W / 2 / 16, H / 2 / 16, 16);

  bp::Arena scratch(32 << 20);
  bp::ThreadPool pool(2);
  bp::Buffer<uint16_t> merged(W, H);

  // 1) 동일 프레임 N장 → bit-exact
  std::vector<float> wmap;
  auto st = bp::merge_burst(b, 0, fields, bp::MergeParams{}, pool, scratch, merged.img, &wmap);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) assert(merged.img.at(x, y) == bufs[0].img.at(x, y));
  std::printf("identical: mean_weight=%.3f expected_diff=%.2f\n", st.mean_weight, st.expected_diff);
  assert(st.mean_weight > 0.99f);
  assert(wmap.size() == (size_t)((W + 15) / 16) * ((H + 15) / 16));

  // 2) 프레임 2를 전혀 다른 내용 + 큰 오차로 → 무시되어야 함 (merged == ref ±1)
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) bufs[2].img.at(x, y) = 4000;
  for (float& e : fields[2].err) e = 1e6f;
  scratch.reset();
  st = bp::merge_burst(b, 0, fields, bp::MergeParams{}, pool, scratch, merged.img);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) assert(std::abs((int)merged.img.at(x, y) - (int)bufs[0].img.at(x, y)) <= 1);
  std::printf("rejected: mean_weight=%.3f\n", st.mean_weight);
  assert(st.mean_weight < 0.7f && st.mean_weight > 0.6f);  // 3장 중 2장만 채택 → 0.667

  // 3) 한 프레임을 2 Bayer px(=gray 1px) 이동 + 모션 필드가 그걸 알려주면 → 여전히 bit-exact (CFA 위상 유지)
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      int sx = x - 2; sx = sx < 0 ? sx + 2 : sx;  // 왼쪽 2열은 같은 위상 값으로 채움
      bufs[2].img.at(x, y) = bufs[0].img.at(sx, y);
    }
  for (size_t i = 0; i < fields[2].err.size(); ++i) { fields[2].err[i] = 0; fields[2].dx[i] = 1; }
  scratch.reset();
  st = bp::merge_burst(b, 0, fields, bp::MergeParams{}, pool, scratch, merged.img);
  int bad = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W - 32; ++x) bad += merged.img.at(x, y) != bufs[0].img.at(x, y);
  std::printf("shifted: mismatches=%d mean_weight=%.3f\n", bad, st.mean_weight);
  assert(bad == 0);
  // 4) 엉터리 노이즈 프로파일(a=1)은 무시 → 정렬 오차 기반 추정으로 대체
  {
    bp::BurstMeta m; m.noise_a = 1.0f; m.noise_b = 1e-6f;
    assert(!bp::noise_profile_plausible(m));
    m.noise_a = 6e-4f; m.noise_b = 2.6e-6f;
    assert(bp::noise_profile_plausible(m));
    m.noise_a = 0; m.noise_b = 0;
    assert(!bp::noise_profile_plausible(m));
  }
  std::puts("test_merge OK");
  return 0;
}
