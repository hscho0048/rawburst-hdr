// LiteRT(TFLite) C API 세그멘터. BP_HAVE_LITERT(arm64 + third_party/litert)일 때만 컴파일된다.
// 인터프리터·델리게이트는 앱 생명주기 동안 1회 생성 (셔터마다 만들면 GPU 커널 컴파일로 지연이 10배).
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstring>
#include <vector>
#include "burstpipe/seg.h"
#include <algorithm>
#include <cstdint>
#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/c_api_experimental.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/gpu/delegate.h"
#if defined(BP_HAVE_QNN)
#include <dlfcn.h>
#include <cstdlib>
#include "TFLiteDelegate/QnnTFLiteDelegate.h"
#endif
#if defined(BP_ANDROID)
#include <android/log.h>
#define SEG_LOG(...) __android_log_print(ANDROID_LOG_WARN, "burstpipe", __VA_ARGS__)
#else
#define SEG_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif
#include <cstdio>

namespace bp {
namespace {

// ---- MediaPipe 커스텀 op: Convolution2DTransposeBias ------------------------------------------------
// MediaPipe selfie_segmenter는 표준 TFLite에 없는 이 op를 쓴다 (MediaPipe 런타임이 자체 등록).
// 입력: x[N,H,W,Cin], w[Cout,kh,kw,Cin], bias[Cout]. custom_initial_data = {padding, stride_w, stride_h} (int32, TfLitePadding)
// GPU 델리게이트는 이 op를 자체 구현으로 가져가므로, 이 CPU 커널은 CPU(XNNPACK) 경로와 GPU 미지원 노드용.
struct TConvParams { int32_t padding, stride_w, stride_h; };
constexpr int32_t kPaddingValid = 2;  // TfLitePadding: Unknown=0 Same=1 Valid=2 (builtin_op_data.h)

bool read_tconv_params(const TfLiteNode* node, TConvParams& p) {
  if (!node->custom_initial_data || node->custom_initial_data_size < (int)sizeof(TConvParams)) return false;
  std::memcpy(&p, node->custom_initial_data, sizeof p);
  return p.stride_w > 0 && p.stride_h > 0;
}

TfLiteStatus TConvPrepare(TfLiteContext* ctx, TfLiteNode* node) {
  if (node->inputs->size != 3 || node->outputs->size != 1) return kTfLiteError;
  const TfLiteTensor& in = ctx->tensors[node->inputs->data[0]];
  const TfLiteTensor& w = ctx->tensors[node->inputs->data[1]];
  TfLiteTensor& out = ctx->tensors[node->outputs->data[0]];
  TConvParams p;
  if (!read_tconv_params(node, p) || in.type != kTfLiteFloat32 || w.type != kTfLiteFloat32) return kTfLiteError;
  if (in.dims->size != 4 || w.dims->size != 4 || w.dims->data[3] != in.dims->data[3]) return kTfLiteError;
  const int H = in.dims->data[1], W = in.dims->data[2], kh = w.dims->data[1], kw = w.dims->data[2];
  const bool valid = p.padding == kPaddingValid;
  TfLiteIntArray* sz = TfLiteIntArrayCreate(4);
  sz->data[0] = in.dims->data[0];
  sz->data[1] = valid ? (H - 1) * p.stride_h + kh : H * p.stride_h;
  sz->data[2] = valid ? (W - 1) * p.stride_w + kw : W * p.stride_w;
  sz->data[3] = w.dims->data[0];
  return ctx->ResizeTensor(ctx, &out, sz);
}

TfLiteStatus TConvInvoke(TfLiteContext* ctx, TfLiteNode* node) {
  const TfLiteTensor& in = ctx->tensors[node->inputs->data[0]];
  const TfLiteTensor& w = ctx->tensors[node->inputs->data[1]];
  const TfLiteTensor& bias = ctx->tensors[node->inputs->data[2]];
  TfLiteTensor& out = ctx->tensors[node->outputs->data[0]];
  TConvParams p;
  read_tconv_params(node, p);
  const int N = in.dims->data[0], H = in.dims->data[1], W = in.dims->data[2], Ci = in.dims->data[3];
  const int Co = w.dims->data[0], kh = w.dims->data[1], kw = w.dims->data[2];
  const int OH = out.dims->data[1], OW = out.dims->data[2];
  // TF transpose conv 패딩: 총 = max((in-1)*stride + k - out, 0), 앞쪽 = 총/2
  const int pad_h = std::max(0, (H - 1) * p.stride_h + kh - OH) / 2;
  const int pad_w = std::max(0, (W - 1) * p.stride_w + kw - OW) / 2;
  const float* x = in.data.f; const float* wt = w.data.f; float* o = out.data.f;
  const float* b = bias.type == kTfLiteFloat32 && bias.data.f ? bias.data.f : nullptr;
  for (int n = 0; n < N; ++n) {
    float* on = o + (size_t)n * OH * OW * Co;
    for (int i = 0; i < OH * OW; ++i)
      for (int c = 0; c < Co; ++c) on[(size_t)i * Co + c] = b ? b[c] : 0.f;
    for (int iy = 0; iy < H; ++iy)
      for (int ix = 0; ix < W; ++ix) {
        const float* xp = x + (((size_t)n * H + iy) * W + ix) * Ci;
        for (int ky = 0; ky < kh; ++ky) {
          const int oy = iy * p.stride_h + ky - pad_h;
          if (oy < 0 || oy >= OH) continue;
          for (int kx = 0; kx < kw; ++kx) {
            const int ox = ix * p.stride_w + kx - pad_w;
            if (ox < 0 || ox >= OW) continue;
            float* op = on + ((size_t)oy * OW + ox) * Co;
            for (int c = 0; c < Co; ++c) {
              const float* wp = wt + (((size_t)c * kh + ky) * kw + kx) * Ci;
              float s = 0;
              for (int k = 0; k < Ci; ++k) s += xp[k] * wp[k];
              op[c] += s;
            }
          }
        }
      }
  }
  return kTfLiteOk;
}

const TfLiteRegistration* tconv_bias_registration() {
  static TfLiteRegistration r = [] {
    TfLiteRegistration x{};
    x.prepare = TConvPrepare;
    x.invoke = TConvInvoke;
    x.custom_name = "Convolution2DTransposeBias";
    x.version = 1;
    return x;
  }();
  return &r;
}

class LiteRtSegmenter : public Segmenter {
 public:
  ~LiteRtSegmenter() override {
    if (interp_) TfLiteInterpreterDelete(interp_);
    if (opts_) TfLiteInterpreterOptionsDelete(opts_);
    if (gpu_) TfLiteGpuDelegateV2Delete(gpu_);
#if defined(BP_HAVE_QNN)
    if (npu_ && qnn_delete_) qnn_delete_(npu_);
#endif
    if (model_) TfLiteModelDelete(model_);
  }
  bool init(const std::string& path, Delegate d, int threads) {
    model_ = TfLiteModelCreateFromFile(path.c_str());
    if (!model_) return false;
    opts_ = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(opts_, threads);  // kCpu: 기본 XNNPACK
    TfLiteInterpreterOptionsAddCustomOp(opts_, "Convolution2DTransposeBias", tconv_bias_registration(), 1, 1);
    if (d == Delegate::kGpu) {
      TfLiteGpuDelegateOptionsV2 go = TfLiteGpuDelegateOptionsV2Default();
      go.inference_preference = TFLITE_GPU_INFERENCE_PREFERENCE_SUSTAINED_SPEED;
      go.inference_priority1 = TFLITE_GPU_INFERENCE_PRIORITY_MIN_LATENCY;
      go.is_precision_loss_allowed = 1;  // fp16
      gpu_ = TfLiteGpuDelegateV2Create(&go);
      if (!gpu_) return false;
      TfLiteInterpreterOptionsAddDelegate(opts_, gpu_);
    }
    if (d == Delegate::kNpu && !add_qnn_htp(path)) return false;
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
    TfLiteStatus st;
    if ((st = TfLiteTensorCopyFromBuffer(in, rgb256, 256 * 256 * 3 * 4)) != kTfLiteOk) { SEG_LOG("seg: copy in failed %d\n", (int)st); return false; }
    if ((st = TfLiteInterpreterInvoke(interp_)) != kTfLiteOk) { SEG_LOG("seg: invoke failed %d\n", (int)st); return false; }
    const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(interp_, 0);
    if ((st = TfLiteTensorCopyToBuffer(out, tmp_.data(), tmp_.size() * 4)) != kTfLiteOk) { SEG_LOG("seg: copy out failed %d\n", (int)st); return false; }
    if (out_ch_ == 1) std::memcpy(mask256, tmp_.data(), 256 * 256 * 4);
    else for (int i = 0; i < 256 * 256; ++i) mask256[i] = tmp_[2 * i + 1];
    return true;
  }
 private:
  // QNN TFLite 델리게이트 → Hexagon HTP (SM7450 = v69). .so는 dlopen (없으면 NPU만 실패, 나머지 경로는 무관).
  // skel(DSP 쪽 라이브러리) 위치: BP_QNN_SKEL_DIR 환경변수 > ADSP_LIBRARY_PATH > 실행 파일 디렉터리.
  bool add_qnn_htp(const std::string& model_path) {
#if defined(BP_HAVE_QNN)
    void* h = dlopen("libQnnTFLiteDelegate.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { SEG_LOG("seg: dlopen libQnnTFLiteDelegate.so failed: %s\n", dlerror()); return false; }
    auto opt_default = (TfLiteQnnDelegateOptions(*)())dlsym(h, "TfLiteQnnDelegateOptionsDefault");
    auto create = (TfLiteDelegate * (*)(const TfLiteQnnDelegateOptions*)) dlsym(h, "TfLiteQnnDelegateCreate");
    qnn_delete_ = (void (*)(TfLiteDelegate*))dlsym(h, "TfLiteQnnDelegateDelete");
    if (!opt_default || !create || !qnn_delete_) { SEG_LOG("seg: QNN delegate symbols missing\n"); return false; }
    TfLiteQnnDelegateOptions o = opt_default();
    o.backend_type = kHtpBackend;
    o.htp_options.precision = kHtpFp16;            // float16 모델 → HTP fp16 (양자화 모델이면 kHtpQuantized)
    o.htp_options.performance_mode = kHtpBurst;    // 셔터 순간 추론: 지연 우선
    const char* skel = std::getenv("BP_QNN_SKEL_DIR");
    if (!skel) skel = std::getenv("ADSP_LIBRARY_PATH");
    if (skel) o.skel_library_dir = skel;
    (void)model_path;
    npu_ = create(&o);
    if (!npu_) { SEG_LOG("seg: TfLiteQnnDelegateCreate failed\n"); return false; }
    TfLiteInterpreterOptionsAddDelegate(opts_, npu_);
    return true;
#else
    (void)model_path;
    SEG_LOG("seg: built without QNN (scripts/fetch_qnn.sh)\n");
    return false;
#endif
  }
#if defined(BP_HAVE_QNN)
  TfLiteDelegate* npu_ = nullptr;
  void (*qnn_delete_)(TfLiteDelegate*) = nullptr;
#endif
  TfLiteModel* model_ = nullptr;
  TfLiteInterpreterOptions* opts_ = nullptr;
  TfLiteDelegate* gpu_ = nullptr;
  TfLiteInterpreter* interp_ = nullptr;
  int out_ch_ = 1;
  std::vector<float> tmp_;
};

// 델리게이트 소유 스레드 (설계문서 6장): GPU 델리게이트는 "초기화한 스레드에서만 Invoke" 가능하다
// (앱에서 실측: "GpuDelegate must run on the same thread where it was initialized").
// 생성·추론·해제를 전부 이 전용 스레드에서 하고, run()은 작업을 넘기고 결과를 기다린다.
class ThreadBoundSegmenter : public Segmenter {
 public:
  ~ThreadBoundSegmenter() override {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }
  bool start(const std::string& path, Delegate d, int threads) {
    th_ = std::thread([this, path, d, threads] {
      auto s = std::make_unique<LiteRtSegmenter>();
      const bool ok = s->init(path, d, threads);
      std::unique_lock<std::mutex> lk(m_);
      init_ok_ = ok; ready_ = true;
      cv_.notify_all();
      if (!ok) return;
      for (;;) {
        cv_.wait(lk, [&] { return stop_ || has_job_; });
        if (stop_) break;
        has_job_ = false;
        lk.unlock();
        const bool r = s->run(in_, out_);
        lk.lock();
        result_ = r; done_ = true;
        cv_.notify_all();
      }
      lk.unlock();
      s.reset();  // 델리게이트 해제도 같은 스레드에서
    });
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return ready_; });
    return init_ok_;
  }
  bool run(const float* rgb256, float* mask256) override {
    std::unique_lock<std::mutex> lk(m_);
    in_ = rgb256; out_ = mask256; done_ = false; has_job_ = true;
    cv_.notify_all();
    cv_.wait(lk, [&] { return done_; });
    return result_;
  }
 private:
  std::thread th_;
  std::mutex m_;
  std::condition_variable cv_;
  const float* in_ = nullptr;
  float* out_ = nullptr;
  bool ready_ = false, init_ok_ = false, has_job_ = false, done_ = false, result_ = false, stop_ = false;
};

}  // namespace

std::unique_ptr<Segmenter> Segmenter::create(const std::string& path, Delegate d, int threads, double* init_ms) {
  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<Segmenter> s;
  if (d == Delegate::kEmu) {
    s = create_emulated(path, 0);
  } else {
    auto l = std::make_unique<ThreadBoundSegmenter>();
    if (l->start(path, d, threads)) s = std::move(l);
  }
  if (init_ms) *init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return s;
}

}  // namespace bp
