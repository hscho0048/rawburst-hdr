#include "ndk_camera.h"
#include <android/log.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#define NLOG(...) __android_log_print(ANDROID_LOG_INFO, "burstpipe", __VA_ARGS__)

namespace bp {
namespace {

bool entry(const ACameraMetadata* m, uint32_t tag, ACameraMetadata_const_entry& e) {
  return ACameraMetadata_getConstEntry(m, tag, &e) == ACAMERA_OK && e.count > 0;
}

struct FrameResult { int64_t exposure = 0; int32_t iso = 0; std::string wb, ccm, noise, black; };

class NdkCameraImpl : public NdkCamera {
 public:
  ~NdkCameraImpl() override {
    if (session_) { ACameraCaptureSession_stopRepeating(session_); ACameraCaptureSession_close(session_); }
    if (preview_req_) ACaptureRequest_free(preview_req_);
    if (device_) ACameraDevice_close(device_);
    if (container_) ACaptureSessionOutputContainer_free(container_);
    if (out_preview_) ACaptureSessionOutput_free(out_preview_);
    if (out_raw_) ACaptureSessionOutput_free(out_raw_);
    if (tgt_preview_) ACameraOutputTarget_free(tgt_preview_);
    if (tgt_raw_) ACameraOutputTarget_free(tgt_raw_);
    if (reader_) AImageReader_delete(reader_);
    if (chars_) ACameraMetadata_free(chars_);
    if (mgr_) ACameraManager_delete(mgr_);
  }

  bool init(ANativeWindow* preview, int n, std::string* info) {
    n_ = n;
    mgr_ = ACameraManager_create();
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr_, &ids) != ACAMERA_OK) return fail(info, "id list");
    // 후면 + RAW 우선
    std::string pick;
    for (int i = 0; i < ids->numCameras && pick.empty(); ++i) {
      ACameraMetadata* m = nullptr;
      if (ACameraManager_getCameraCharacteristics(mgr_, ids->cameraIds[i], &m) != ACAMERA_OK) continue;
      ACameraMetadata_const_entry e;
      bool raw = false, back = false;
      if (entry(m, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, e))
        for (uint32_t k = 0; k < e.count; ++k) raw |= e.data.u8[k] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW;
      if (entry(m, ACAMERA_LENS_FACING, e)) back = e.data.u8[0] == ACAMERA_LENS_FACING_BACK;
      if (raw && back) { pick = ids->cameraIds[i]; chars_ = m; } else ACameraMetadata_free(m);
    }
    ACameraManager_deleteCameraIdList(ids);
    if (pick.empty()) return fail(info, "no back RAW camera");
    id_ = pick;
    ACameraMetadata_const_entry e;
    if (entry(chars_, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, e))
      for (uint32_t k = 0; k + 3 < e.count; k += 4)
        if (e.data.i32[k] == AIMAGE_FORMAT_RAW16 && e.data.i32[k + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
            (int64_t)e.data.i32[k + 1] * e.data.i32[k + 2] > (int64_t)w_ * h_) { w_ = e.data.i32[k + 1]; h_ = e.data.i32[k + 2]; }
    if (!w_) return fail(info, "no RAW16 size");
    if (entry(chars_, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, e))
      for (uint32_t k = 0; k < e.count; ++k) manual_ |= e.data.u8[k] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR;
    build_static_meta();

    slots_.assign(n_, std::vector<uint16_t>((size_t)w_ * h_));
    if (AImageReader_new(w_, h_, AIMAGE_FORMAT_RAW16, n_ + 2, &reader_) != AMEDIA_OK) return fail(info, "image reader");
    AImageReader_ImageListener il{this, &NdkCameraImpl::on_image};
    AImageReader_setImageListener(reader_, &il);
    ANativeWindow* raw_win = nullptr;
    AImageReader_getWindow(reader_, &raw_win);

    ACameraDevice_StateCallbacks dcb{this, [](void*, ACameraDevice*) { NLOG("ndk camera disconnected"); },
                                     [](void*, ACameraDevice*, int err) { NLOG("ndk camera error %d", err); }};
    if (ACameraManager_openCamera(mgr_, id_.c_str(), &dcb, &device_) != ACAMERA_OK) return fail(info, "openCamera (권한?)");
    ACaptureSessionOutputContainer_create(&container_);
    ACaptureSessionOutput_create(preview, &out_preview_);
    ACaptureSessionOutput_create(raw_win, &out_raw_);
    ACaptureSessionOutputContainer_add(container_, out_preview_);
    ACaptureSessionOutputContainer_add(container_, out_raw_);
    ACameraOutputTarget_create(preview, &tgt_preview_);
    ACameraOutputTarget_create(raw_win, &tgt_raw_);
    ACameraCaptureSession_stateCallbacks scb{this, [](void*, ACameraCaptureSession*) {}, [](void*, ACameraCaptureSession*) {},
                                            [](void*, ACameraCaptureSession*) {}};
    if (ACameraDevice_createCaptureSession(device_, container_, &scb, &session_) != ACAMERA_OK) return fail(info, "session");
    ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &preview_req_);
    ACaptureRequest_addTarget(preview_req_, tgt_preview_);
    static ACameraCaptureSession_captureCallbacks pcb{};
    pcb.context = this;
    pcb.onCaptureCompleted = &NdkCameraImpl::on_preview_result;
    if (ACameraCaptureSession_setRepeatingRequest(session_, &pcb, 1, &preview_req_, nullptr) != ACAMERA_OK) return fail(info, "preview");
    char buf[160];
    std::snprintf(buf, sizeof buf, "ndk camera %s raw %dx%d manual=%d cfa=%d", id_.c_str(), w_, h_, (int)manual_, cfa_);
    if (info) *info = buf;
    NLOG("%s", buf);
    return true;
  }

  bool shoot(NdkBurst& out, int timeout_ms) override {
    auto t0 = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(m_);
      count_ = 0; results_.clear(); ts_.assign(n_, 0); active_ = true;
    }
    ACaptureRequest* req = nullptr;
    ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &req);
    ACaptureRequest_addTarget(req, tgt_raw_);
    uint8_t nr = ACAMERA_NOISE_REDUCTION_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_NOISE_REDUCTION_MODE, 1, &nr);
    if (manual_) {
      int64_t expo = (int64_t)(last_expo_ / 2.83);  // EV −1.5
      int32_t iso = last_iso_;
      const int64_t max_expo = 33333333;            // 1/30s
      if (expo > max_expo) { iso = (int32_t)((int64_t)iso * expo / max_expo); expo = max_expo; }
      ACameraMetadata_const_entry e;
      if (entry(chars_, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, e)) iso = std::min(std::max(iso, e.data.i32[0]), e.data.i32[1]);
      if (entry(chars_, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, e)) expo = std::min(std::max(expo, e.data.i64[0]), e.data.i64[1]);
      uint8_t ae = ACAMERA_CONTROL_AE_MODE_OFF;
      ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
      ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &expo);
      ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
      NLOG("ndk burst manual expo=%lldns iso=%d", (long long)expo, iso);
    } else {
      uint8_t lock = ACAMERA_CONTROL_AE_LOCK_ON;
      int32_t comp = -4;  // ≈ −1.5EV @ 1/3 step
      ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_LOCK, 1, &lock);
      ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &comp);
    }
    std::vector<ACaptureRequest*> reqs(n_, req);
    static ACameraCaptureSession_captureCallbacks ccb{};
    ccb.context = this;
    ccb.onCaptureCompleted = &NdkCameraImpl::on_still_result;
    ccb.onCaptureFailed = [](void* ctx, ACameraCaptureSession*, ACaptureRequest*, ACameraCaptureFailure*) {
      auto* s = static_cast<NdkCameraImpl*>(ctx); std::lock_guard<std::mutex> lk(s->m_); ++s->failed_; s->cv_.notify_all();
    };
    failed_ = 0;
    const camera_status_t st = ACameraCaptureSession_capture(session_, &ccb, n_, reqs.data(), nullptr);
    ACaptureRequest_free(req);
    if (st != ACAMERA_OK) return false;
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return count_ + failed_ >= n_ && (int)results_.size() >= count_; });
    active_ = false;
    if (count_ == 0) return false;
    // 타임스탬프 순 정렬 + meta 문자열
    std::vector<int> order(count_);
    for (int i = 0; i < count_; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ts_[a] < ts_[b]; });
    out.width = w_; out.height = h_; out.count = count_;
    out.frames.clear();
    for (int i : order) out.frames.push_back(slots_[i].data());
    static const FrameResult kNone;
    const FrameResult& fr0 = results_.count(ts_[order[0]]) ? results_[ts_[order[0]]] : results_.empty() ? kNone : results_.begin()->second;
    std::ostringstream s;
    s << static_meta_;
    if (!fr0.black.empty()) s << "black_level " << fr0.black << "\n"; else s << static_black_;
    if (!fr0.wb.empty()) s << "wb_gains " << fr0.wb << "\n";
    s << "ccm " << (fr0.ccm.empty() ? fm_ccm_ : fr0.ccm) << "\n";
    if (!fr0.noise.empty()) s << "noise_profile " << fr0.noise << "\n";
    for (int k = 0; k < count_; ++k) {
      const int64_t t = ts_[order[k]];
      auto it = results_.find(t);
      s << "frame " << k << " " << t << " " << (it != results_.end() ? it->second.exposure : 0) << " "
        << (it != results_.end() ? it->second.iso : 0) << "\n";
    }
    out.meta_text = s.str();
    out.capture_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    NLOG("ndk burst frames=%d failed=%d results=%zu capture=%.0fms", count_, failed_, results_.size(), out.capture_ms);
    return true;
  }

  int raw_width() const override { return w_; }
  int raw_height() const override { return h_; }

 private:
  static bool fail(std::string* info, const char* m) { if (info) *info = std::string("ndk camera: ") + m; NLOG("ndk camera: %s", m); return false; }

  // 정적 메타: 크기·CFA·화이트·블랙 패턴·orientation, CCM 대체용 ForwardMatrix(D65) × XYZ(D50)→sRGB
  void build_static_meta() {
    ACameraMetadata_const_entry e;
    int white = 1023;
    if (entry(chars_, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, e)) cfa_ = e.data.u8[0];
    if (entry(chars_, ACAMERA_SENSOR_INFO_WHITE_LEVEL, e)) white = e.data.i32[0];
    int orient = 0;
    if (entry(chars_, ACAMERA_SENSOR_ORIENTATION, e)) orient = e.data.i32[0];
    std::ostringstream s;
    s << "width " << w_ << "\nheight " << h_ << "\ncfa " << cfa_ << "\nwhite_level " << white << "\norientation " << orient << "\n";
    static_meta_ = s.str();
    if (entry(chars_, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, e) && e.count >= 4) {
      std::ostringstream b; b << "black_level " << e.data.i32[0] << " " << e.data.i32[1] << " " << e.data.i32[2] << " " << e.data.i32[3] << "\n";
      static_black_ = b.str();
    }
    // ForwardMatrix: 기준 광원이 D65(21)인 쪽
    uint32_t fm_tag = ACAMERA_SENSOR_FORWARD_MATRIX1;
    if (entry(chars_, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, e) && e.data.u8[0] != 21 &&
        entry(chars_, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, e) && e.data.u8[0] == 21) fm_tag = ACAMERA_SENSOR_FORWARD_MATRIX2;
    double fm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (entry(chars_, fm_tag, e) && e.count >= 9)
      for (int i = 0; i < 9; ++i) fm[i] = (double)e.data.r[i].numerator / e.data.r[i].denominator;
    const double xyz2srgb[9] = {3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427};
    std::ostringstream c;
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) {
        double v = 0; for (int j = 0; j < 3; ++j) v += xyz2srgb[r * 3 + j] * fm[j * 3 + k];
        c << (r || k ? " " : "") << v;
      }
    fm_ccm_ = c.str();
  }

  static void on_image(void* ctx, AImageReader* r) {
    auto* s = static_cast<NdkCameraImpl*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(r, &img) != AMEDIA_OK || !img) return;
    std::lock_guard<std::mutex> lk(s->m_);
    if (s->active_ && s->count_ < s->n_) {
      uint8_t* data = nullptr; int len = 0, stride = 0;
      AImage_getPlaneData(img, 0, &data, &len);
      AImage_getPlaneRowStride(img, 0, &stride);
      int64_t ts = 0; AImage_getTimestamp(img, &ts);
      uint16_t* dst = s->slots_[s->count_].data();
      const size_t row = (size_t)s->w_ * 2;
      for (int y = 0; y < s->h_; ++y) std::memcpy(dst + (size_t)y * s->w_, data + (size_t)y * stride, row);  // 1회 복사
      s->ts_[s->count_] = ts;
      ++s->count_;
      s->cv_.notify_all();
    }
    AImage_delete(img);  // HAL 버퍼 즉시 반납 (오래 잡으면 프레임 드롭)
  }

  static void on_preview_result(void* ctx, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata* res) {
    auto* s = static_cast<NdkCameraImpl*>(ctx);
    ACameraMetadata_const_entry e;
    if (entry(res, ACAMERA_SENSOR_EXPOSURE_TIME, e)) s->last_expo_ = e.data.i64[0];
    if (entry(res, ACAMERA_SENSOR_SENSITIVITY, e)) s->last_iso_ = e.data.i32[0];
  }

  static void on_still_result(void* ctx, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata* res) {
    auto* s = static_cast<NdkCameraImpl*>(ctx);
    ACameraMetadata_const_entry e;
    FrameResult fr;
    int64_t ts = 0;
    if (entry(res, ACAMERA_SENSOR_TIMESTAMP, e)) ts = e.data.i64[0];
    if (entry(res, ACAMERA_SENSOR_EXPOSURE_TIME, e)) fr.exposure = e.data.i64[0];
    if (entry(res, ACAMERA_SENSOR_SENSITIVITY, e)) fr.iso = e.data.i32[0];
    if (entry(res, ACAMERA_COLOR_CORRECTION_GAINS, e) && e.count >= 4) {
      std::ostringstream o; o << e.data.f[0] << " " << e.data.f[1] << " " << e.data.f[2] << " " << e.data.f[3]; fr.wb = o.str();
    }
    if (entry(res, ACAMERA_COLOR_CORRECTION_TRANSFORM, e) && e.count >= 9) {
      double v[9]; bool ident = true;
      for (int i = 0; i < 9; ++i) { v[i] = (double)e.data.r[i].numerator / e.data.r[i].denominator; ident &= std::fabs(v[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 1e-3; }
      if (!ident) { std::ostringstream o; for (int i = 0; i < 9; ++i) o << (i ? " " : "") << v[i]; fr.ccm = o.str(); }
    }
    if (entry(res, ACAMERA_SENSOR_NOISE_PROFILE, e) && e.count >= 2) {
      double a = 0, b = 0; const int nch = e.count / 2;
      for (int i = 0; i < nch; ++i) { a += e.data.d[2 * i]; b += e.data.d[2 * i + 1]; }
      std::ostringstream o; o << a / nch << " " << b / nch; fr.noise = o.str();
    }
    if (entry(res, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL, e) && e.count >= 4) {
      std::ostringstream o; o << e.data.f[0] << " " << e.data.f[1] << " " << e.data.f[2] << " " << e.data.f[3]; fr.black = o.str();
    }
    std::lock_guard<std::mutex> lk(s->m_);
    s->results_[ts] = fr;
    s->cv_.notify_all();
  }

  int n_ = 8, w_ = 0, h_ = 0, cfa_ = 0;
  bool manual_ = false;
  std::string id_, static_meta_, static_black_, fm_ccm_;
  ACameraManager* mgr_ = nullptr;
  ACameraMetadata* chars_ = nullptr;
  ACameraDevice* device_ = nullptr;
  AImageReader* reader_ = nullptr;
  ACaptureSessionOutputContainer* container_ = nullptr;
  ACaptureSessionOutput *out_preview_ = nullptr, *out_raw_ = nullptr;
  ACameraOutputTarget *tgt_preview_ = nullptr, *tgt_raw_ = nullptr;
  ACameraCaptureSession* session_ = nullptr;
  ACaptureRequest* preview_req_ = nullptr;
  std::vector<std::vector<uint16_t>> slots_;
  std::vector<int64_t> ts_;
  std::map<int64_t, FrameResult> results_;
  std::mutex m_;
  std::condition_variable cv_;
  int count_ = 0, failed_ = 0;
  bool active_ = false;
  int64_t last_expo_ = 16666666;
  int32_t last_iso_ = 400;
};

}  // namespace

std::unique_ptr<NdkCamera> NdkCamera::open(ANativeWindow* preview, int n, std::string* info) {
  auto c = std::make_unique<NdkCameraImpl>();
  if (!c->init(preview, n, info)) return nullptr;
  return c;
}

}  // namespace bp
