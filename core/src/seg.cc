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

// x^(1/2.2) on [0,1] — 픽셀마다 std::pow를 부르지 않도록 4096 LUT (모델 입력 정밀도로 충분)
static float gamma_lut(float x) {
  static const std::vector<float> lut = [] {
    std::vector<float> t(4097);
    for (int i = 0; i <= 4096; ++i) t[i] = std::pow(i / 4096.f, 1.f / 2.2f);
    return t;
  }();
  const float f = std::min(1.f, std::max(0.f, x)) * 4096.f;
  const int i = std::min(4095, (int)f);
  return lut[i] + (f - i) * (lut[i + 1] - lut[i]);
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
      for (int c = 0; c < 3; ++c) o[c] = gamma_lut(rgb[c] * ev_gain);
    }
  }
  const int deg = ((m.orientation % 360) + 360) % 360;
  if (deg) {
    std::vector<float> tmp(out, out + 256 * 256 * 3);
    rotate256(tmp.data(), out, 3, deg);
  }
}

void rotate256(const float* src, float* dst, int ch, int deg_cw) {
  // 시계방향 90°: src(x,y) → dst(255−y, x)
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      int dx = x, dy = y;
      switch (deg_cw) {
        case 90: dx = 255 - y; dy = x; break;
        case 180: dx = 255 - x; dy = 255 - y; break;
        case 270: dx = y; dy = 255 - x; break;
        default: break;
      }
      for (int c = 0; c < ch; ++c) dst[(dy * 256 + dx) * ch + c] = src[(y * 256 + x) * ch + c];
    }
}

void mask256_to_sensor(float* mask, int orientation) {
  const int deg = ((orientation % 360) + 360) % 360;
  if (!deg) return;
  std::vector<float> tmp(mask, mask + 256 * 256);
  rotate256(tmp.data(), mask, 1, (360 - deg) % 360);
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
