// LiteRT(TFLite) C API 세그멘터. BP_HAVE_LITERT(arm64 + third_party/litert)일 때만 컴파일된다.
// 인터프리터·델리게이트는 앱 생명주기 동안 1회 생성 (셔터마다 만들면 GPU 커널 컴파일로 지연이 10배).
#include <chrono>
#include <cstring>
#include <vector>
#include "burstpipe/seg.h"
#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/delegates/gpu/delegate.h"

namespace bp {
namespace {

class LiteRtSegmenter : public Segmenter {
 public:
  ~LiteRtSegmenter() override {
    if (interp_) TfLiteInterpreterDelete(interp_);
    if (opts_) TfLiteInterpreterOptionsDelete(opts_);
    if (gpu_) TfLiteGpuDelegateV2Delete(gpu_);
    if (model_) TfLiteModelDelete(model_);
  }
  bool init(const std::string& path, Delegate d, int threads) {
    if (d == Delegate::kNpu) return false;  // QNN HTP: 설계문서 부록 A. 반나절 상한 후 GPU로 확정
    model_ = TfLiteModelCreateFromFile(path.c_str());
    if (!model_) return false;
    opts_ = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(opts_, threads);  // kCpu: 기본 XNNPACK
    if (d == Delegate::kGpu) {
      TfLiteGpuDelegateOptionsV2 go = TfLiteGpuDelegateOptionsV2Default();
      go.inference_preference = TFLITE_GPU_INFERENCE_PREFERENCE_SUSTAINED_SPEED;
      go.inference_priority1 = TFLITE_GPU_INFERENCE_PRIORITY_MIN_LATENCY;
      go.is_precision_loss_allowed = 1;  // fp16
      gpu_ = TfLiteGpuDelegateV2Create(&go);
      if (!gpu_) return false;
      TfLiteInterpreterOptionsAddDelegate(opts_, gpu_);
    }
    interp_ = TfLiteInterpreterCreate(model_, opts_);
    if (!interp_ || TfLiteInterpreterAllocateTensors(interp_) != kTfLiteOk) return false;
    const TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interp_, 0);
    const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(interp_, 0);
    if (TfLiteTensorByteSize(in) != 256 * 256 * 3 * 4) return false;
    out_ch_ = (int)(TfLiteTensorByteSize(out) / (256 * 256 * 4));
    if (out_ch_ != 1 && out_ch_ != 2) return false;
    tmp_.resize((size_t)256 * 256 * out_ch_);
    std::vector<float> warm(256 * 256 * 3, 0.5f), wm(256 * 256);
    return run(warm.data(), wm.data());  // 웜업 (GPU 커널 컴파일 포함)
  }
  bool run(const float* rgb256, float* mask256) override {
    TfLiteTensor* in = TfLiteInterpreterGetInputTensor(interp_, 0);
    if (TfLiteTensorCopyFromBuffer(in, rgb256, 256 * 256 * 3 * 4) != kTfLiteOk) return false;
    if (TfLiteInterpreterInvoke(interp_) != kTfLiteOk) return false;
    const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(interp_, 0);
    if (TfLiteTensorCopyToBuffer(out, tmp_.data(), tmp_.size() * 4) != kTfLiteOk) return false;
    if (out_ch_ == 1) std::memcpy(mask256, tmp_.data(), 256 * 256 * 4);
    else for (int i = 0; i < 256 * 256; ++i) mask256[i] = tmp_[2 * i + 1];
    return true;
  }
 private:
  TfLiteModel* model_ = nullptr;
  TfLiteInterpreterOptions* opts_ = nullptr;
  TfLiteDelegate* gpu_ = nullptr;
  TfLiteInterpreter* interp_ = nullptr;
  int out_ch_ = 1;
  std::vector<float> tmp_;
};

}  // namespace

std::unique_ptr<Segmenter> Segmenter::create(const std::string& path, Delegate d, int threads, double* init_ms) {
  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<Segmenter> s;
  if (d == Delegate::kEmu) {
    s = create_emulated(path, 0);
  } else {
    auto l = std::make_unique<LiteRtSegmenter>();
    if (l->init(path, d, threads)) s = std::move(l);
  }
  if (init_ms) *init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return s;
}

}  // namespace bp
