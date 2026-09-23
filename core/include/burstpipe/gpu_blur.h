#pragma once
#include <memory>
#include <string>
#include "burstpipe/image.h"

namespace bp {

struct GpuBlurTimes { double upload_ms = 0, kernel_ms = 0, download_ms = 0, submit_wait_ms = 0; };

// Vulkan compute 정규화 디스크 블러 (설계문서 L5). disc_blur_normalized()와 같은 입출력 규약.
// Android(BP_ANDROID)에서만 구현. PC 또는 Vulkan 없음/반지름 초과면 create()가 nullptr → 호출자는 CPU 경로.
class GpuBlur {
 public:
  static constexpr int kMaxRadius = 12;  // 셰이더 공유 메모리 타일 크기
  virtual ~GpuBlur() = default;
  // qw×qh 선형 RGB(qw*3 폭) + α(qw) 버퍼를 한 번 만든다.
  static std::unique_ptr<GpuBlur> create(int qw, int qh, std::string* why);
  virtual bool run(const Image<float>& rgb, const Image<float>& alpha, int radius, Image<float>& out, GpuBlurTimes* t) = 0;
  virtual std::string device_name() const = 0;
};

}  // namespace bp
