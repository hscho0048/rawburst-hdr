// 플랫폼 무관 부분: RAW → 모델 입력, 델리게이트 이름, 에뮬레이션 세그멘터.
// LiteRT 구현체는 seg_litert.cc (arm64 + third_party/litert 일 때만 컴파일).
#include "burstpipe/seg.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <vector>

namespace bp {

const char* delegate_name(Delegate d) {
  switch (d) {
    case Delegate::kCpu: return "cpu";
    case Delegate::kGpu: return "gpu";
    case Delegate::kNpu: return "npu";
    case Delegate::kEmu: return "emu";
  }
  return "?";
}

bool parse_delegate(const std::string& s, Delegate& d) {
  if (s == "cpu") d = Delegate::kCpu;
  else if (s == "gpu") d = Delegate::kGpu;
  else if (s == "npu") d = Delegate::kNpu;
  else if (s == "emu") d = Delegate::kEmu;
  else return false;
  return true;
}

void bayer_to_rgb256(const Image<uint16_t>& b, const BurstMeta& m, float ev_gain, float* out) {
  const int SW = b.w / 2, SH = b.h / 2;  // 슈퍼픽셀 격자
  for (int j = 0; j < 256; ++j) {
    int sy = std::min(SH - 1, (int)((j + 0.5f) * SH / 256));
    for (int i = 0; i < 256; ++i) {
      int sx = std::min(SW - 1, (int)((i + 0.5f) * SW / 256));
      float ch[4] = {0, 0, 0, 0};  // R Gr Gb B
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          int x = 2 * sx + dx, y = 2 * sy + dy;
          float bl = m.black_at(x, y);
          ch[kCfaColor[m.cfa][dy * 2 + dx]] = std::min(1.f, std::max(0.f, (b.at(x, y) - bl) * m.gain_at(x, y) / (m.white_level - bl)));
        }
      float rgb[3] = {ch[0], 0.5f * (ch[1] + ch[2]), ch[3]};
      float* o = out + (j * 256 + i) * 3;
      for (int c = 0; c < 3; ++c) o[c] = std::pow(std::min(1.f, rgb[c] * ev_gain), 1.f / 2.2f);
    }
  }
}

namespace {

// 입력을 무시하고 미리 계산된 마스크를 돌려준다. 파이프라인의 스레드·합류·보케 경로를 PC에서 검증하는 용도.
class EmuSegmenter : public Segmenter {
 public:
  EmuSegmenter(std::vector<float> mask, double latency_ms) : mask_(std::move(mask)), latency_ms_(latency_ms) {}
  bool run(const float*, float* mask256) override {
    if (latency_ms_ > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(latency_ms_));
    std::copy(mask_.begin(), mask_.end(), mask256);
    return true;
  }
 private:
  std::vector<float> mask_;
  double latency_ms_;
};

}  // namespace

std::unique_ptr<Segmenter> Segmenter::create_emulated(const std::string& mask_path, double latency_ms) {
  std::vector<float> m(256 * 256);
  std::ifstream f(mask_path, std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(m.data()), (std::streamsize)(m.size() * 4))) return nullptr;
  return std::make_unique<EmuSegmenter>(std::move(m), latency_ms);
}

#if !defined(BP_HAVE_LITERT)
std::unique_ptr<Segmenter> Segmenter::create(const std::string& model_path, Delegate d, int, double* init_ms) {
  if (init_ms) *init_ms = 0;
  if (d == Delegate::kEmu) return create_emulated(model_path, 0);
  return nullptr;  // LiteRT 없음 (PC 빌드 또는 third_party/litert 미설치)
}
#endif

}  // namespace bp
