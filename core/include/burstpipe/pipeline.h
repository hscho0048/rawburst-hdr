#pragma once
#include <memory>
#include <vector>
#include "burstpipe/align.h"
#include "burstpipe/bokeh.h"
#include "burstpipe/burst.h"
#include "burstpipe/finish.h"
#include "burstpipe/merge.h"
#include "burstpipe/seg.h"
#include "burstpipe/thread_pool.h"
#include "burstpipe/timings.h"

namespace bp {

struct PipelineParams {
  AlignParams align; MergeParams merge; FinishParams finish; BokehParams bokeh;
  int threads = 4; std::vector<int> cpus; int consider_ref = 3;
  int seg_cpu = -1;  // ≥0이면 세그 스레드를 이 코어에 고정 (big 코어 경쟁 회피 실험용)
};

struct PipelineOutput {
  Image<uint8_t> rgba;           // (W*4, H) 최종 (보케 적용 시 합성 결과)
  Image<uint16_t> merged;        // (W, H) 합성된 Bayer
  Image<float> alpha;            // (W/4, H/4) 정제된 마스크. 보케 안 했으면 empty
  std::vector<float> merge_weights;  // 합성 타일 격자의 alt 평균 가중치 (고스트 시각화)
  int weights_w = 0, weights_h = 0;
  int ref = 0;
  MergeStats merge_stats;
  bool bokeh = false;
};

// 셔터 1회 = run 1회. 모든 큰 버퍼는 생성자에서 1회 할당하고 run 사이에 재사용한다.
class Pipeline {
 public:
  Pipeline(int w, int h, int max_frames, const PipelineParams& p);   // max_frames ≤ kMaxFrames
  // mask256 != nullptr 이면 그것을 쓰고, 아니면 seg != nullptr 일 때 별도 스레드에서 추론(정렬·합성 뒤에 숨김),
  // 둘 다 없으면 보케 생략. 반환 이미지는 Pipeline 소유, 다음 run까지 유효.
  const PipelineOutput& run(const Burst& b, const float* mask256, Segmenter* seg, Timings& t);
  ThreadPool& pool() { return pool_; }
  size_t scratch_capacity() const { return scratch_.capacity(); }
  size_t scratch_peak() const { return scratch_.peak(); }
  size_t persistent_capacity() const { return persistent_.capacity(); }
 private:
  int w_, h_, max_frames_;
  PipelineParams p_;
  ThreadPool pool_;
  Arena persistent_, scratch_;
  Image<float> rgb_lin_q_, alpha_;
  std::vector<float> rgb256_, mask_buf_;
  PipelineOutput out_;
};

}  // namespace bp
