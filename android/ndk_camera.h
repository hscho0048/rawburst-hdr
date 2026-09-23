#pragma once
// Camera2 NDK 캡처 (설계문서 부록 A "Camera2 NDK 포팅"). Kotlin CaptureController/BurstDumper와 같은 일을 C++에서:
//  - 후면 + RAW 카메라, 스트림 2개(프리뷰 ANativeWindow + AImageReader RAW16 N+2)
//  - HDR+ 노출: 버스트 전 프레임 같은 수동 노출 EV −1.5, 1/30s 상한 (MANUAL_SENSOR 없으면 AE lock + 보정)
//  - AImage 평면 → 네이티브 슬롯 memcpy 1회 (Java ByteBuffer 경유 없음), 결과 메타는 SENSOR_TIMESTAMP로 매칭
//  - meta.txt와 같은 BurstMeta 문자열을 만든다 (CCM: 결과가 단위행렬이면 ForwardMatrix, orientation 포함)
#include <android/native_window.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bp {

struct NdkBurst {
  int width = 0, height = 0, count = 0;
  std::vector<uint16_t*> frames;  // 타임스탬프 순, 슬롯 소유는 NdkCamera
  std::string meta_text;          // parse_meta 입력
  double capture_ms = 0;
};

class NdkCamera {
 public:
  static std::unique_ptr<NdkCamera> open(ANativeWindow* preview, int n, std::string* info);
  virtual ~NdkCamera() = default;
  // 버스트 1회 캡처 (블로킹, timeout_ms). 성공 시 out은 다음 shoot까지 유효.
  virtual bool shoot(NdkBurst& out, int timeout_ms) = 0;
  virtual int raw_width() const = 0;
  virtual int raw_height() const = 0;
};

}  // namespace bp
