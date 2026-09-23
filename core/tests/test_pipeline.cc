// 최상위 수용 테스트: 센서 에뮬레이터 버스트 → Pipeline::run 전체.
// 개별 stage 테스트가 "부품이 맞다"를 보장하면, 이 테스트는 "조립했을 때 설계 목표가 나온다"를 본다.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "burstpipe/pipeline.h"
#include "sensor_emu.h"

namespace {

struct EmuBurst {
  bp::emu::SensorEmu emu;
  std::vector<bp::Buffer<uint16_t>> bufs;
  bp::Burst burst;
  explicit EmuBurst(const bp::emu::EmuParams& p, bp::ThreadPool& pool) : emu(p) {
    burst.meta = emu.meta();
    for (int i = 0; i < p.frames; ++i) {
      bufs.emplace_back(p.width, p.height);
      emu.render(i, bufs.back().img, true, pool);
    }
    for (auto& b : bufs) burst.frames.push_back(b.img);
  }
};

// 같은 CFA 위상(2칸 간격) 표준편차
double phase_std(const bp::Image<uint16_t>& im, const int r[4]) {
  double s = 0, s2 = 0; long n = 0;
  for (int y = r[1]; y < r[1] + r[3]; y += 2)
    for (int x = r[0]; x < r[0] + r[2]; x += 2) { double v = im.at(x, y); s += v; s2 += v * v; ++n; }
  double m = s / n;
  return std::sqrt(std::max(0.0, s2 / n - m * m));
}

double rmse(const bp::Image<uint16_t>& a, const bp::Image<uint16_t>& b, const int r[4]) {
  double s = 0; long n = 0;
  for (int y = r[1]; y < r[1] + r[3]; ++y)
    for (int x = r[0]; x < r[0] + r[2]; ++x) { double d = (double)a.at(x, y) - b.at(x, y); s += d * d; ++n; }
  return std::sqrt(s / n);
}

}  // namespace

int main() {
  bp::ThreadPool gen_pool(4);
  bp::PipelineParams pp;
  pp.threads = 4;

  // ---- 1) 정적 장면: SNR 이득 + 회색 카드 색 ----
  bp::emu::EmuParams sp; sp.scene = bp::emu::Scene::kStatic; sp.width = 1024; sp.height = 768; sp.frames = 8; sp.shake_px = 1.5f;
  EmuBurst st(sp, gen_pool);
  bp::Pipeline pipe(sp.width, sp.height, sp.frames, pp);
  bp::Timings t;
  const bp::PipelineOutput& o = pipe.run(st.burst, nullptr, nullptr, t);
  // 평탄 영역은 흔들림만큼 움직였으므로 ref 프레임 기준으로 쓴다 (에뮬 truth는 frame 0 기준 + 여유)
  const int* flat = st.emu.truth().flat;
  double s_single = phase_std(st.burst.frames[o.ref], flat), s_merged = phase_std(o.merged, flat);
  double gain_db = 20 * std::log10(s_single / s_merged);
  std::printf("[static] ref=%d mean_weight=%.3f single_std=%.2f merged_std=%.2f SNR gain=%.2f dB (theory %.2f)\n", o.ref,
              o.merge_stats.mean_weight, s_single, s_merged, gain_db, 20 * std::log10(std::sqrt(8.0)));
  assert(o.merge_stats.mean_weight > 0.8f);
  assert(gain_db > 6.0);
  {
    const int cx = (flat[0] + flat[2] / 2) & ~1, cy = (flat[1] + flat[3] / 2) & ~1;
    int acc[3] = {0, 0, 0};
    for (int dy = -8; dy < 8; ++dy)
      for (int dx = -8; dx < 8; ++dx)
        for (int c = 0; c < 3; ++c) acc[c] += o.rgba.at(4 * (cx + dx) + c, cy + dy);
    for (int& a : acc) a /= 256;
    std::printf("[static] gray card rgb = %d %d %d\n", acc[0], acc[1], acc[2]);
    assert(std::abs(acc[0] - acc[1]) <= 6 && std::abs(acc[2] - acc[1]) <= 6);
    assert(acc[1] > 90 && acc[1] < 170);  // 18% 회색 → EV 복원 후 중간톤
  }
  // 삼각대(흔들림 0): 합성 결과가 노이즈 없는 기대값에 8장 평균 수준으로 가깝다 (√8 → ×0.35).
  // 흔들림이 있으면 v1은 정수(gray) 정렬이라 강한 엣지에 ±1px 잔차 블러가 남는다 — 설계상 알려진 한계(서브픽셀 v2).
  {
    bp::emu::EmuParams tp = sp; tp.shake_px = 0;
    EmuBurst tri(tp, gen_pool);
    bp::Timings tt;
    const bp::PipelineOutput& ot = pipe.run(tri.burst, nullptr, nullptr, tt);
    bp::Buffer<uint16_t> clean(tp.width, tp.height);
    tri.emu.render(ot.ref, clean.img, false, gen_pool);
    int inner[4] = {0, 0, tp.width, tp.height};
    double e1 = rmse(tri.burst.frames[ot.ref], clean.img, inner), e8 = rmse(ot.merged, clean.img, inner);
    std::printf("[tripod] RMSE vs clean: single=%.2f merged=%.2f DN (ideal x%.2f)\n", e1, e8, 1 / std::sqrt(8.0));
    assert(e8 < e1 * 0.45);
  }

  // ---- 2) 움직임 장면: 손 영역은 거부(고스트 없음). 주의: o/om 등은 Pipeline 소유 버퍼 참조라 다음 run이 덮어쓴다 ----
  bp::emu::EmuParams mp = sp; mp.scene = bp::emu::Scene::kMotion;
  EmuBurst mv(mp, gen_pool);
  bp::Timings tm;
  const bp::PipelineOutput& om = pipe.run(mv.burst, nullptr, nullptr, tm);
  {
    bp::Buffer<uint16_t> clean(mp.width, mp.height);
    mv.emu.render(om.ref, clean.img, false, gen_pool);
    const int* r = mv.emu.truth().moving;
    int mr[4] = {std::max(0, r[0]), std::max(0, r[1]), std::min(r[2], mp.width - std::max(0, r[0])), std::min(r[3], mp.height - std::max(0, r[1]))};
    double e1 = rmse(mv.burst.frames[om.ref], clean.img, mr), e8 = rmse(om.merged, clean.img, mr);
    // 합성 타일 가중치 맵: 손이 지나간 영역 안 vs 밖
    const int S = pp.merge.tile / 2;
    double win = 0, wout = 0; int nin = 0, nout = 0;
    for (int ty = 0; ty < om.weights_h; ++ty)
      for (int tx = 0; tx < om.weights_w; ++tx) {
        const int cx = tx * S + S, cy = ty * S + S;
        const bool inside = cx >= mr[0] && cx < mr[0] + mr[2] && cy >= mr[1] && cy < mr[1] + mr[3];
        (inside ? win : wout) += om.merge_weights[(size_t)ty * om.weights_w + tx];
        ++(inside ? nin : nout);
      }
    win /= nin; wout /= nout;
    std::printf("[motion] ref=%d mean_weight=%.3f weight in/out moving region=%.3f/%.3f RMSE(moving region) single=%.2f merged=%.2f\n",
                om.ref, om.merge_stats.mean_weight, win, wout, e1, e8);
    assert(win < 0.7 * wout);
    assert(e8 < e1 * 1.1);  // 고스트면 손 윤곽이 겹쳐 수십~수백 DN 오차
  }

  // ---- 3) 인물 장면: 에뮬 세그 스레드 경로 + 보케 ----
  bp::emu::EmuParams pp_ = sp; pp_.scene = bp::emu::Scene::kPortrait;
  EmuBurst pt(pp_, gen_pool);
  std::vector<float> mask(256 * 256);
  pt.emu.seg_mask256(mask.data());
  const std::string mpath = "test_pipeline_mask.bin";
  { FILE* f = std::fopen(mpath.c_str(), "wb"); std::fwrite(mask.data(), 4, mask.size(), f); std::fclose(f); }
  auto seg = bp::Segmenter::create_emulated(mpath, 5.0);
  std::remove(mpath.c_str());
  assert(seg);
  // 보케 없는 결과 복사
  bp::Timings t0;
  const bp::PipelineOutput& o0 = pipe.run(pt.burst, nullptr, nullptr, t0);
  std::vector<uint8_t> sharp(o0.rgba.data, o0.rgba.data + (size_t)o0.rgba.w * o0.rgba.h);
  bp::Timings tb;
  const bp::PipelineOutput& ob = pipe.run(pt.burst, nullptr, seg.get(), tb);
  assert(ob.bokeh && !ob.alpha.empty());
  double seg_wait = -1;
  for (auto& s : tb.ms) if (s.first == "seg_wait") seg_wait = s.second;
  std::printf("[portrait] seg_wait=%.2f ms (emu latency 5 ms, hidden behind align+merge)\n", seg_wait);
  assert(seg_wait >= 0 && seg_wait < 5.0);
  // 얼굴 중앙(전경)은 그대로, 배경은 바뀐다
  const int W = pp_.width, H = pp_.height;
  auto px_diff = [&](int x, int y) {
    int d = 0;
    for (int c = 0; c < 3; ++c) d += std::abs((int)ob.rgba.at(4 * x + c, y) - (int)sharp[(size_t)y * W * 4 + 4 * x + c]);
    return d;
  };
  long fg = 0, bg = 0;
  for (int dy = -10; dy < 10; ++dy) for (int dx = -10; dx < 10; ++dx) { fg += px_diff(W / 2 + dx, (int)(0.38f * H) + dy); bg += px_diff(W / 10 + dx, H / 5 + dy); }
  std::printf("[portrait] mean |Δ| face=%.2f background=%.2f\n", fg / 400.0, bg / 400.0);
  assert(fg / 400.0 < 1.0 && bg / 400.0 > 3.0);
  // 정제된 α vs GT (머리카락 포함) IoU
  {
    std::vector<float> gt((size_t)ob.alpha.w * ob.alpha.h);
    pt.emu.alpha_gt(ob.alpha.w, ob.alpha.h, gt.data());
    std::vector<float> raw((size_t)ob.alpha.w * ob.alpha.h);
    bp::Image<float> m{256, 256, 256, mask.data()}, rim{ob.alpha.w, ob.alpha.h, ob.alpha.w, raw.data()};
    bp::resize_bilinear(m, rim);
    auto iou = [&](const float* a) {
      long in = 0, un = 0;
      for (size_t i = 0; i < gt.size(); ++i) { bool p = a[i] > 0.5f, g = gt[i] > 0.5f; in += p && g; un += p || g; }
      return (double)in / un;
    };
    double iou_raw = iou(raw.data()), iou_ref = iou(ob.alpha.data);
    std::printf("[portrait] IoU vs GT: upsampled mask=%.4f guided=%.4f\n", iou_raw, iou_ref);
    assert(iou_ref > 0.9);
  }

  // ---- 4) 1장 경로 ----
  bp::Burst one = st.burst; one.frames.resize(1); one.meta.frames.resize(1);
  bp::Timings t1;
  const bp::PipelineOutput& o1 = pipe.run(one, nullptr, nullptr, t1);
  assert(o1.ref == 0 && o1.merge_weights.empty());

  std::puts("test_pipeline OK");
  return 0;
}
