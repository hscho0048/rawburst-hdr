// 초해상도형 합성 평가: 에뮬 버스트(노이즈 포함) → 정답 RGB(ref 프레임, 모자이크 전)에 같은 CCM·톤을 적용한 8bit와 PSNR.
// 비교: 단일+Malvar / 공간 합성+Malvar / Wiener+Malvar / 초해상도(디모자이크 겸). 손떨림 있음·삼각대.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "burstpipe/finish.h"
#include "burstpipe/merge.h"
#include "burstpipe/superres.h"
#include "sensor_emu.h"

namespace {
double psnr_rgba(const bp::Image<uint8_t>& a, const std::vector<uint8_t>& ref, int W, int H, int b) {
  double se = 0; long n = 0;
  for (int y = b; y < H - b; ++y)
    for (int x = b; x < W - b; ++x)
      for (int c = 0; c < 3; ++c) { double d = (double)a.at(4 * x + c, y) - ref[((size_t)y * W + x) * 3 + c]; se += d * d; ++n; }
  return 10 * std::log10(255.0 * 255.0 / (se / n));
}
void to8(const float* lin, const float* C, const bp::ToneLut& lut, size_t npx, uint8_t* out, int stride4) {
  for (size_t i = 0; i < npx; ++i) {
    const float r = lin[3 * i], g = lin[3 * i + 1], b = lin[3 * i + 2];
    uint8_t* o = out + i * stride4;
    o[0] = lut(std::max(0.f, C[0] * r + C[1] * g + C[2] * b));
    o[1] = lut(std::max(0.f, C[3] * r + C[4] * g + C[5] * b));
    o[2] = lut(std::max(0.f, C[6] * r + C[7] * g + C[8] * b));
  }
}
}  // namespace

int main() {
  bp::ThreadPool pool(8);
  for (float shake : {1.5f, 0.f}) {
    bp::emu::EmuParams ep; ep.scene = bp::emu::Scene::kStatic; ep.width = 1024; ep.height = 768; ep.frames = 8; ep.shake_px = shake;
    bp::emu::SensorEmu emu(ep);
    const int W = ep.width, H = ep.height, N = ep.frames;
    std::vector<bp::Buffer<uint16_t>> bufs;
    bp::Burst b; b.meta = emu.meta();
    for (int i = 0; i < N; ++i) { bufs.emplace_back(W, H); emu.render(i, bufs.back().img, true, pool); }
    for (auto& x : bufs) b.frames.push_back(x.img);
    // 정렬 (소수 이동 포함)
    bp::Arena arena(256 << 20);
    std::vector<bp::Pyramid> pyrs(N);
    for (int i = 0; i < N; ++i) { auto g = arena.alloc<uint16_t>(W / 2, H / 2); bp::bayer_to_gray(b.frames[i], g); bp::build_pyramid(g, 4, arena, pyrs[i]); }
    const int ref = bp::select_reference(pyrs, 3);
    std::vector<bp::MotionField> fields(N);
    bp::AlignParams ap; ap.subpixel = true;
    for (int i = 0; i < N; ++i) if (i != ref) bp::align_frame(pyrs[ref], pyrs[i], ap, pool, fields[i]);
    // 정답
    bp::FinishParams fp; bp::ToneLut lut(fp);
    std::vector<float> truth((size_t)W * H * 3);
    emu.render_rgb_truth(ref, truth.data(), pool);
    std::vector<uint8_t> ref8((size_t)W * H * 3);
    to8(truth.data(), b.meta.ccm, lut, (size_t)W * H, ref8.data(), 3);
    auto finish_psnr = [&](const bp::Image<uint16_t>& bayer) {
      bp::Buffer<uint8_t> rgba(W * 4, H); bp::Buffer<float> q((W / 4) * 3, H / 4);
      bp::Arena s((size_t)W * H * 4 + (8 << 20));
      bp::finish(bayer, b.meta, fp, pool, s, rgba.img, q.img);
      return psnr_rgba(rgba.img, ref8, W, H, 8);
    };
    const double p_single = finish_psnr(b.frames[ref]);
    bp::Buffer<uint16_t> merged(W, H);
    bp::Arena ms((size_t)W * H * 8 + (8 << 20));
    bp::merge_burst(b, ref, fields, bp::MergeParams{}, pool, ms, merged.img);
    const double p_spatial = finish_psnr(merged.img);
    ms.reset();
    bp::MergeParams wp; wp.mode = bp::MergeMode::kWiener; wp.wiener_c = 16;
    bp::merge_burst_wiener(b, ref, fields, wp, pool, ms, merged.img);
    const double p_wiener = finish_psnr(merged.img);
    bp::Buffer<float> sr(W * 3, H);
    bp::Buffer<uint8_t> sr8(W * 4, H);
    auto run_sr = [&](const bp::SrParams& sp) {
      bp::merge_superres(b, ref, fields, sp, pool, sr.img);
      to8(sr.v.data(), b.meta.ccm, lut, (size_t)W * H, sr8.v.data(), 4);
      return psnr_rgba(sr8.img, ref8, W, H, 8);
    };
    // 파라미터 탐색 (환경변수 BP_SR_SWEEP=1)
    if (std::getenv("BP_SR_SWEEP")) {
      for (float sg : {0.5f, 0.7f}) for (float srb : {0.8f, 1.1f}) for (float kst : {1.f, 2.f, 3.f}) for (float ksh : {0.35f, 0.6f, 1.f}) {
        bp::SrParams sp; sp.sigma_g = sg; sp.sigma_rb = srb; sp.k_stretch = kst; sp.k_shrink = ksh; sp.anisotropic = !(kst == 1.f && ksh == 1.f);
        std::printf("  sweep shake=%.1f sg=%.1f srb=%.1f stretch=%.1f shrink=%.2f → %.2f dB\n", shake, sg, srb, kst, ksh, run_sr(sp));
      }
    }
    const double p_sr = run_sr(bp::SrParams{});
    std::printf("shake=%.1f PSNR vs truth: single+malvar %.2f | spatial+malvar %.2f | wiener+malvar %.2f | superres %.2f dB\n",
                shake, p_single, p_spatial, p_wiener, p_sr);
    std::fflush(stdout);
    assert(p_sr > p_single);
    if (shake > 0) assert(p_sr > p_spatial);  // 손떨림(서브픽셀 다양성)이 있으면 공간 합성+Malvar보다 낫다
  }
  std::puts("test_superres OK");
  return 0;
}
