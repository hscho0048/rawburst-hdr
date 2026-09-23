#include "burstpipe/pipeline.h"
#include "burstpipe/superres.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace bp {

// persistent: merged(2) + rgba(4) /픽셀, 1/4 해상도 rgb_lin_q(12) + alpha(4)
static size_t persistent_bytes(int w, int h) {
  return (size_t)w * h * (2 + 4) + (size_t)(w / 4) * (h / 4) * (12 + 4) + (1 << 20);
}
// scratch는 stage 사이에 reset되므로 stage별 최대값
static size_t scratch_bytes(int w, int h, int n, bool superres) {
  size_t wh = (size_t)w * h;
  size_t align = (size_t)n * (wh / 4) * 2 * 4 / 3 + (size_t)n * 64 * 8 + (1 << 20);  // gray 피라미드 (등비합 4/3)
  size_t merge = wh * 8;                                          // num + den
  size_t finish = wh * 4 + (size_t)(w / 4) * (h / 4) * 4 * 4;       // lin + (ltm) 1/4 RGB + 게인
  size_t bokeh = (size_t)(w / 4 + 1) * (h / 4) * 4 * 18;          // alpha0, guide + guided 7장 + blur 3ch + blur_rgba + 누적합 4ch
  if (superres) merge = wh * 12;                                  // 풀해상도 RGB float
  return std::max({align, merge, finish, bokeh}) + (8 << 20);
}

Pipeline::Pipeline(int w, int h, int max_frames, const PipelineParams& p)
    : w_(w), h_(h), max_frames_(std::max(1, std::min(max_frames, kMaxFrames))), p_(p), pool_(p.threads, p.cpus),
      persistent_(persistent_bytes(w, h)), scratch_(scratch_bytes(w, h, max_frames_, p.merge.mode == MergeMode::kSuperRes)) {
  out_.merged = persistent_.alloc<uint16_t>(w, h);
  out_.rgba = persistent_.alloc<uint8_t>(w * 4, h);
  rgb_lin_q_ = persistent_.alloc<float>((w / 4) * 3, h / 4);
  alpha_ = persistent_.alloc<float>(w / 4, h / 4);
  rgb256_.resize(256 * 256 * 3);
  mask_buf_.resize(256 * 256);
  if (p.gpu_blur) {
    std::string why;
    gpu_ = GpuBlur::create(w / 4, h / 4, &why);
    gpu_status_ = gpu_ ? "vulkan " + gpu_->device_name() : "unavailable (" + why + ")";
  }
}

const PipelineOutput& Pipeline::run(const Burst& b, const float* mask256, Segmenter* seg, Timings& t) {
  const int N = std::min((int)b.frames.size(), max_frames_);
  if (b.meta.width != w_ || b.meta.height != h_ || N <= 0) std::abort();  // 호출자 계약 위반
  scratch_.reset();

  // 0) 세그멘테이션: 입력이 frame 0 RAW 축소본이라 정렬·합성을 기다릴 필요가 없다 → 임계 경로 밖
  double seg_ms = 0, seg_prep_ms = 0;
  bool seg_ok = false;
  std::thread seg_thread;
  if (!mask256 && seg) {
    seg_thread = std::thread([&] {
      if (p_.seg_cpu >= 0) pin_current_thread(p_.seg_cpu);
      auto t0 = std::chrono::steady_clock::now();
      bayer_to_rgb256(b.frames[0], b.meta, p_.finish.ev_gain, rgb256_.data());
      auto t1 = std::chrono::steady_clock::now();
      seg_prep_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      seg_ok = seg->run(rgb256_.data(), mask_buf_.data());
      if (seg_ok) mask256_to_sensor(mask_buf_.data(), b.meta.orientation);  // 정립 좌표 → 센서 좌표
      seg_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    });
  }

  // 1) gray 피라미드
  std::vector<Pyramid> pyrs(N);
  {
    BP_STAGE(t, "gray_pyramid");
    std::vector<Image<uint16_t>> grays(N);
    for (int i = 0; i < N; ++i) grays[i] = scratch_.alloc<uint16_t>(w_ / 2, h_ / 2);   // 할당은 순차
    pool_.parallel_for(N, [&](int i) { bayer_to_gray(b.frames[i], grays[i]); });
    for (int i = 0; i < N; ++i) build_pyramid(grays[i], p_.align.nlevels, scratch_, pyrs[i]);
  }
  // 2) 참조 프레임 (처음 consider_ref장 중 가장 선명)
  int ref;
  { BP_STAGE(t, "select_ref"); ref = select_reference(pyrs, p_.consider_ref); }
  // 3) 타일 정렬
  std::vector<MotionField> fields(N);
  {
    BP_STAGE(t, "align");
    AlignParams ap = p_.align;
    if (p_.merge.mode == MergeMode::kSuperRes) ap.subpixel = true;  // 초해상도는 소수 이동이 핵심
    for (int i = 0; i < N; ++i) if (i != ref) align_frame(pyrs[ref], pyrs[i], ap, pool_, fields[i]);
  }
  // 4) 강건 합성 (Bayer 도메인)
  scratch_.reset();
  Burst bb = b; bb.frames.resize(N); bb.meta.frames.resize(N);
  {
    BP_STAGE(t, "merge");
    if (N == 1) {
      for (int y = 0; y < h_; ++y) std::copy(b.frames[0].row(y), b.frames[0].row(y) + w_, out_.merged.row(y));
      out_.merge_stats = MergeStats{};
      out_.merge_weights.clear();
    } else {
      if (p_.merge.mode == MergeMode::kSuperRes) {  // Bayer 결과는 덤프·통계용: 가벼운 공간 합성 대신 참조 프레임 복사
        for (int y = 0; y < h_; ++y) std::copy(b.frames[ref].row(y), b.frames[ref].row(y) + w_, out_.merged.row(y));
        out_.merge_stats = MergeStats{};
      } else
      out_.merge_stats = p_.merge.mode == MergeMode::kWiener
                             ? merge_burst_wiener(bb, ref, fields, p_.merge, pool_, scratch_, out_.merged, &out_.merge_weights)
                             : merge_burst(bb, ref, fields, p_.merge, pool_, scratch_, out_.merged, &out_.merge_weights);
    }
    const int S = p_.merge.tile / 2;
    out_.weights_w = N == 1 ? 0 : (w_ + S - 1) / S;
    out_.weights_h = N == 1 ? 0 : (h_ + S - 1) / S;
  }
  // 5) 마무리: WB → 디모자이크 → CCM → 톤 → sRGB
  scratch_.reset();
  if (p_.merge.mode == MergeMode::kSuperRes && N > 1) {
    // 초해상도: 합성과 디모자이크를 한 번에 (merge 단계는 위에서 공간 합성 결과를 Bayer 덤프용으로만 남김)
    Image<float> rgb = scratch_.alloc<float>(w_ * 3, h_);
    { BP_STAGE(t, "superres"); merge_superres(bb, ref, fields, SrParams{}, pool_, rgb); }
    { BP_STAGE(t, "finish"); finish_rgb(rgb, b.meta, p_.finish, pool_, out_.rgba, rgb_lin_q_); }
  } else {
    BP_STAGE(t, "finish");
    finish(out_.merged, b.meta, p_.finish, pool_, scratch_, out_.rgba, rgb_lin_q_);
  }
  scratch_.reset();

  // 6) 세그 합류. 정상이면 seg_wait ≈ 0 (합성 뒤에 숨음)
  if (seg_thread.joinable()) {
    {
      BP_STAGE(t, "seg_wait");
      seg_thread.join();
    }
    t.add("seg_prep_parallel", seg_prep_ms);
    t.add("seg_infer_parallel", seg_ms);
    if (seg_ok) mask256 = mask_buf_.data();
  }

  // 7) 인물모드: 마스크 정제 → 정규화 디스크 블러 → 합성
  out_.bokeh = mask256 != nullptr;
  out_.alpha = Image<float>{};
  if (mask256) {
    const int QW = w_ / 4, QH = h_ / 4;
    Image<float> alpha0 = scratch_.alloc<float>(QW, QH), guide = scratch_.alloc<float>(QW, QH);
    Image<float>& alpha = alpha_;
    {
      BP_STAGE(t, "mask_refine");
      Image<float> m{256, 256, 256, const_cast<float*>(mask256)};
      resize_bilinear(m, alpha0);
      luma_of(rgb_lin_q_, p_.finish.ev_gain, guide);
      guided_filter(guide, alpha0, p_.bokeh.gf_radius, p_.bokeh.gf_eps, scratch_, alpha);
      for (int i = 0; i < QW * QH; ++i) alpha.data[i] = std::min(1.f, std::max(0.f, alpha.data[i]));
    }
    Image<float> blur = scratch_.alloc<float>(QW * 3, QH);
    {
      BP_STAGE(t, "disc_blur");
      highlight_boost(rgb_lin_q_, p_.finish.ev_gain, p_.bokeh.highlight);
      GpuBlurTimes gt;
      if (gpu_ && p_.bokeh.radius <= GpuBlur::kMaxRadius && gpu_->run(rgb_lin_q_, alpha, p_.bokeh.radius, blur, &gt)) {
        t.add("disc_blur.upload", gt.upload_ms);
        t.add("disc_blur.gpu_submit_wait", gt.submit_wait_ms);
        if (gt.kernel_ms >= 0) t.add("disc_blur.gpu_kernel", gt.kernel_ms);
        t.add("disc_blur.download", gt.download_ms);
      } else {
        disc_blur_normalized(rgb_lin_q_, alpha, p_.bokeh.radius, pool_, scratch_, blur);
      }
    }
    {
      BP_STAGE(t, "composite");
      Image<uint8_t> blur_rgba = scratch_.alloc<uint8_t>(QW * 4, QH);
      ToneLut lut(p_.finish);
      rgb_lin_to_rgba8(blur, lut, pool_, blur_rgba);
      composite(out_.rgba, blur_rgba, alpha, pool_, out_.rgba);
    }
    out_.alpha = alpha;
  }
  out_.ref = ref;
  return out_;
}

}  // namespace bp
