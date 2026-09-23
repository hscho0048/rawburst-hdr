// JNI: Kotlin 앱 → core/ Pipeline. 프레임은 direct ByteBuffer 주소를 그대로 Image<uint16_t>로 감싼다 (복사 없음).
#include <android/log.h>
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "burstpipe/pipeline.h"
#include "burstpipe/seg.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "burstpipe", __VA_ARGS__)

namespace {
std::mutex g_mu;
std::unique_ptr<bp::Pipeline> g_pipe;
std::unique_ptr<bp::Segmenter> g_seg;
bp::PipelineParams g_params;
int g_w = 0, g_h = 0;

std::string with_extra(const bp::Timings& t, const bp::PipelineOutput& o, double wall_ms) {
  std::string js = t.json();
  js.pop_back();  // '}'
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s\"total\":%.3f,\"wall\":%.3f,\"ref\":%d,\"mean_weight\":%.4f,\"bokeh\":%d}",
                t.ms.empty() ? "" : ",", t.total(), wall_ms, o.ref, o.merge_stats.mean_weight, (int)o.bokeh);
  return js + buf;
}

bool copy_out(JNIEnv* env, jobject outRgba, const bp::Image<uint8_t>& rgba) {
  auto* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(outRgba));
  const size_t row = (size_t)rgba.w;  // w*4
  if (!dst || env->GetDirectBufferCapacity(outRgba) < (jlong)(row * rgba.h)) return false;
  for (int y = 0; y < rgba.h; ++y) std::memcpy(dst + (size_t)y * row, rgba.row(y), row);
  return true;
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_burstpipe_Native_init(JNIEnv* env, jobject, jint w, jint h, jint maxFrames, jstring modelPath, jint delegate,
                               jint threads, jintArray cpus) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_params = bp::PipelineParams{};
  g_params.threads = threads;
  jsize nc = cpus ? env->GetArrayLength(cpus) : 0;
  if (nc > 0) { std::vector<jint> c(nc); env->GetIntArrayRegion(cpus, 0, nc, c.data()); g_params.cpus.assign(c.begin(), c.end()); }
  g_w = w; g_h = h;
  g_pipe = std::make_unique<bp::Pipeline>(w, h, maxFrames, g_params);
  std::string status = "seg: none";
  g_seg.reset();
  if (modelPath) {
    const char* s = env->GetStringUTFChars(modelPath, nullptr);
    double init_ms = 0;
    bp::Delegate d = delegate == 1 ? bp::Delegate::kGpu : delegate == 2 ? bp::Delegate::kNpu : bp::Delegate::kCpu;
    g_seg = bp::Segmenter::create(s, d, 4, &init_ms);
    char buf[128];
    std::snprintf(buf, sizeof buf, "seg[%s]: %s init %.0f ms", bp::delegate_name(d),
                  g_seg ? "ok" : "unavailable (LiteRT 미포함 빌드)", init_ms);
    status = buf;
    env->ReleaseStringUTFChars(modelPath, s);
  }
  LOGI("init %dx%d frames=%d threads=%d %s", w, h, maxFrames, threads, status.c_str());
  return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_burstpipe_Native_process(JNIEnv* env, jobject, jobjectArray frames, jint n, jstring metaTxt, jobject outRgba) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_pipe || n <= 0) return env->NewStringUTF("");
  bp::Burst b;
  const char* ms = env->GetStringUTFChars(metaTxt, nullptr);
  std::istringstream is(ms);
  bool ok = bp::parse_meta(is, b.meta);
  env->ReleaseStringUTFChars(metaTxt, ms);
  if (!ok || b.meta.width != g_w || b.meta.height != g_h) { LOGI("meta mismatch"); return env->NewStringUTF(""); }
  for (int i = 0; i < n; ++i) {
    jobject buf = env->GetObjectArrayElement(frames, i);
    auto* p = static_cast<uint16_t*>(env->GetDirectBufferAddress(buf));
    jlong cap = env->GetDirectBufferCapacity(buf);
    env->DeleteLocalRef(buf);
    if (!p || cap < (jlong)g_w * g_h * 2) return env->NewStringUTF("");
    b.frames.push_back(bp::Image<uint16_t>{g_w, g_h, g_w, p});
  }
  b.meta.frames.resize(n);
  bp::Timings t;
  auto t0 = std::chrono::steady_clock::now();
  const bp::PipelineOutput& o = g_pipe->run(b, nullptr, g_seg.get(), t);
  double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!copy_out(env, outRgba, o.rgba)) return env->NewStringUTF("");
  std::string js = with_extra(t, o, wall);
  LOGI("timings %s", js.c_str());
  return env->NewStringUTF(js.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_burstpipe_Native_processDir(JNIEnv* env, jobject, jstring jdir, jobject outRgba) {
  std::lock_guard<std::mutex> lk(g_mu);
  const char* s = env->GetStringUTFChars(jdir, nullptr);
  std::string dir(s);
  env->ReleaseStringUTFChars(jdir, s);
  bp::BurstMeta meta;
  if (!bp::load_meta(dir + "/meta.txt", meta)) { LOGI("processDir: no meta in %s", dir.c_str()); return env->NewStringUTF(""); }
  const int n = std::min<int>((int)meta.frames.size(), bp::kMaxFrames);
  bp::Arena arena((size_t)meta.width * meta.height * 2 * n + (1 << 20));
  bp::Burst b;
  if (!bp::load_burst(dir, arena, b, n)) return env->NewStringUTF("");
  // 크기가 다를 수 있으므로 별도 파이프라인 (테스트 경로라 할당 비용 무시)
  bp::Pipeline pipe(meta.width, meta.height, n, g_params);
  auto seg = bp::Segmenter::create_emulated(dir + "/mask_emu.bin", 0);
  bp::Segmenter* sp = seg ? seg.get() : g_seg.get();
  bp::Timings t;
  auto t0 = std::chrono::steady_clock::now();
  const bp::PipelineOutput& o = pipe.run(b, nullptr, sp, t);
  double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!copy_out(env, outRgba, o.rgba)) return env->NewStringUTF("");
  std::string js = with_extra(t, o, wall);
  LOGI("processDir %s seg=%s timings %s", dir.c_str(), seg ? "emu" : (g_seg ? "litert" : "none"), js.c_str());
  return env->NewStringUTF(js.c_str());
}
