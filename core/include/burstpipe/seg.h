#pragma once
#include <memory>
#include <string>
#include "burstpipe/burst.h"

namespace bp {

// kEmu: 기기/LiteRT 없이 파이프라인의 세그 스레드 경로를 돌리기 위한 에뮬레이션.
//       model_path 자리에 256×256 float32 마스크 파일(burstpipe_emu가 쓰는 mask_emu.bin)을 받는다.
enum class Delegate { kCpu, kGpu, kNpu, kEmu };

const char* delegate_name(Delegate d);
bool parse_delegate(const std::string& s, Delegate& d);  // "cpu" "gpu" "npu" "emu"

class Segmenter {
 public:
  virtual ~Segmenter() = default;
  // 모델 로드 + 델리게이트 + 웜업 1회. 실패 시 nullptr.
  // kCpu/kGpu/kNpu는 BP_HAVE_LITERT(arm64 + third_party/litert)일 때만 성공한다. kEmu는 모든 플랫폼.
  static std::unique_ptr<Segmenter> create(const std::string& model_path, Delegate d, int threads, double* init_ms);
  // latency_ms: 추론 지연을 흉내 내는 sleep. 병렬 경로(seg_wait) 동작 확인용이지 측정값이 아니다.
  static std::unique_ptr<Segmenter> create_emulated(const std::string& mask_path, double latency_ms);
  virtual bool run(const float* rgb256 /*256*256*3, [0,1]*/, float* mask256 /*256*256*/) = 0;
};

// RAW → 2×2 슈퍼픽셀 → 256×256 RGB (감마 2.2, 비균등 리사이즈) → m.orientation만큼 시계방향 회전(정립). 모델 입력.
// 인물 모델은 정립된 사람을 기대한다 — 센서 방향(폰 세로 = 90°) 그대로 넣으면 옆으로 누운 사람이라 마스크가 반쪽이 된다.
void bayer_to_rgb256(const Image<uint16_t>& bayer, const BurstMeta& m, float ev_gain, float* rgb256);
// 정립 좌표의 256×256 마스크를 센서 좌표로 되돌린다 (in-place, orientation만큼 반시계 회전)
void mask256_to_sensor(float* mask256, int orientation);
// 256×256 × ch 배열을 시계방향 deg(0/90/180/270)만큼 회전. src≠dst
void rotate256(const float* src, float* dst, int ch, int deg_cw);

}  // namespace bp
