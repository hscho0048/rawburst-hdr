#include "sensor_emu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace bp::emu {
namespace {

constexpr float kPi = 3.14159265358979f;

// ---- 결정적 난수 / 절차적 텍스처 ----------------------------------------------------------
inline uint64_t mix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
inline float hash01(int x, int y, uint32_t s) {
  uint64_t h = mix64(((uint64_t)(uint32_t)x << 32) ^ (uint32_t)y ^ ((uint64_t)s << 17));
  return (float)(h >> 40) * (1.f / 16777216.f);
}
inline float smooth(float t) { return t * t * (3.f - 2.f * t); }
float value_noise(float x, float y, uint32_t s) {
  int xi = (int)std::floor(x), yi = (int)std::floor(y);
  float fx = smooth(x - xi), fy = smooth(y - yi);
  float a = hash01(xi, yi, s), b = hash01(xi + 1, yi, s), c = hash01(xi, yi + 1, s), d = hash01(xi + 1, yi + 1, s);
  return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
float fbm(float x, float y, int oct, uint32_t s) {
  float v = 0, amp = 0.5f, norm = 0;
  for (int o = 0; o < oct; ++o) { v += amp * value_noise(x, y, s + o * 101); norm += amp; x *= 2.03f; y *= 2.03f; amp *= 0.5f; }
  return v / norm;
}

struct Rng {  // 행 단위 시드 → 병렬 렌더에서도 결정적
  uint64_t s;
  explicit Rng(uint64_t seed) : s(mix64(seed)) {}
  float uniform() { s = mix64(s); return ((float)(s >> 40) + 0.5f) * (1.f / 16777216.f); }
  float gauss() {  // Box–Muller
    float u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.f * std::log(u1)) * std::cos(2.f * kPi * u2);
  }
};

float srgb8_to_lin(int v) {
  float c = v / 255.f;
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

const int kMacbeth[24][3] = {
    {115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
    {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
    {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
    {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52}};

bool invert3(const float m[9], float o[9]) {
  float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (std::fabs(det) < 1e-9f) return false;
  float id = 1.f / det;
  o[0] = (m[4] * m[8] - m[5] * m[7]) * id; o[1] = (m[2] * m[7] - m[1] * m[8]) * id; o[2] = (m[1] * m[5] - m[2] * m[4]) * id;
  o[3] = (m[5] * m[6] - m[3] * m[8]) * id; o[4] = (m[0] * m[8] - m[2] * m[6]) * id; o[5] = (m[2] * m[3] - m[0] * m[5]) * id;
  o[6] = (m[3] * m[7] - m[4] * m[6]) * id; o[7] = (m[1] * m[6] - m[0] * m[7]) * id; o[8] = (m[0] * m[4] - m[1] * m[3]) * id;
  return true;
}

inline bool in_rect(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }
inline void set3(float o[3], float r, float g, float b) { o[0] = r; o[1] = g; o[2] = b; }

}  // namespace

bool parse_scene(const std::string& s, Scene& out) {
  if (s == "static") out = Scene::kStatic;
  else if (s == "motion") out = Scene::kMotion;
  else if (s == "lowlight") out = Scene::kLowLight;
  else if (s == "portrait") out = Scene::kPortrait;
  else return false;
  return true;
}

const char* scene_name(Scene s) {
  switch (s) {
    case Scene::kStatic: return "static";
    case Scene::kMotion: return "motion";
    case Scene::kLowLight: return "lowlight";
    case Scene::kPortrait: return "portrait";
  }
  return "?";
}

SensorEmu::SensorEmu(const EmuParams& p) : p_(p) {
  // ---- 메타 (Camera2가 돌려줄 값의 그럴듯한 대역) ----
  meta_.width = p.width; meta_.height = p.height; meta_.cfa = p.cfa; meta_.white_level = 1023;
  for (float& v : meta_.black_level) v = 64;
  const float wb[4] = {2.05f, 1.0f, 1.0f, 1.62f};
  const float ccm[9] = {1.58f, -0.41f, -0.17f, -0.22f, 1.44f, -0.22f, 0.02f, -0.55f, 1.53f};
  std::copy(wb, wb + 4, meta_.wb_gains);
  std::copy(ccm, ccm + 9, meta_.ccm);
  invert3(meta_.ccm, cam_from_srgb_);

  int iso = p.iso;
  float expo_ms = 10.f, scene_gain = 1.f;
  switch (p.scene) {
    case Scene::kStatic: iso = iso ? iso : 200; expo_ms = 10.f; break;
    case Scene::kMotion: iso = iso ? iso : 400; expo_ms = 8.f; break;
    case Scene::kLowLight: iso = iso ? iso : 3200; expo_ms = 33.3f; scene_gain = 0.12f; truth_.ev_gain = 2.8f / 0.12f * 0.6f; break;
    case Scene::kPortrait: iso = iso ? iso : 400; expo_ms = 12.f; break;
  }
  // 노이즈 프로파일: base ISO 100에서 full well ≈ 6600e⁻ (a=1.5e-4), 읽기 노이즈는 게인²에 비례 + 고정분
  const float g = iso / 100.f;
  meta_.noise_a = 1.5e-4f * g;
  meta_.noise_b = 1.0e-7f * g * g + 1.0e-6f;
  // EV −1.5 (HDR+ 언더노출): 장면 1.0(확산 백색) → raw 1/2.83
  expo_scale_ = scene_gain / 2.83f;

  // ---- 손떨림: 프레임 간 랜덤워크 (첫 프레임 0) ----
  Rng rng(p.seed * 7919u + 13);
  truth_.shift_x.assign(p.frames, 0.f); truth_.shift_y.assign(p.frames, 0.f);
  for (int i = 1; i < p.frames; ++i) {
    truth_.shift_x[i] = truth_.shift_x[i - 1] + p.shake_px * rng.gauss();
    truth_.shift_y[i] = truth_.shift_y[i - 1] + p.shake_px * rng.gauss();
  }
  const int64_t t0 = 100000000000ll + (int64_t)p.seed * 1000;
  for (int i = 0; i < p.frames; ++i) {
    FrameMeta fm;
    fm.timestamp_ns = t0 + (int64_t)i * 33333333ll;
    fm.exposure_ns = (int64_t)(expo_ms * 1e6f);
    fm.iso = iso;
    meta_.frames.push_back(fm);
  }

  // ---- 정답 영역 ----
  const float H = (float)p.height, A = (float)p.width / p.height;
  if (p.scene != Scene::kPortrait) {
    // 회색 카드 [0.55A,0.75A]×[0.58,0.88] 안쪽, 흔들림 여유 15%
    float x0 = 0.55f * A * H, x1 = 0.75f * A * H, y0 = 0.58f * H, y1 = 0.88f * H;
    float mx = 0.15f * (x1 - x0), my = 0.15f * (y1 - y0);
    truth_.flat[0] = ((int)(x0 + mx)) & ~1; truth_.flat[1] = ((int)(y0 + my)) & ~1;
    truth_.flat[2] = ((int)(x1 - x0 - 2 * mx)) & ~1; truth_.flat[3] = ((int)(y1 - y0 - 2 * my)) & ~1;
  }
  if (p.scene == Scene::kMotion) {
    float cx0 = 0.28f * H, cx1 = (0.28f + 0.025f * (p.frames - 1)) * H;
    truth_.moving[0] = (int)(cx0 - 0.12f * H); truth_.moving[1] = (int)(0.22f * H);
    truth_.moving[2] = (int)(cx1 - cx0 + 0.24f * H); truth_.moving[3] = (int)(0.34f * H);
  }
}

// 인물: 머리 타원 + 목 + 어깨/몸통. with_hair면 머리 윤곽 밖 가는 머리카락 가닥 포함.
// 프레임마다 숨쉬기 정도의 미세 이동 (서브픽셀) → 정렬·합성은 사실상 정적.
float SensorEmu::person_alpha(float u, float v, int frame, bool with_hair) const {
  const float H = (float)p_.height, A = (float)p_.width / p_.height;
  float x = u / H - (0.6f * std::sin(frame * 1.3f)) / H, y = v / H - (0.4f * std::cos(frame * 0.9f)) / H;
  const float cx = 0.5f * A;
  auto head = [&](float xx, float yy) { float dx = (xx - cx) / 0.10f, dy = (yy - 0.36f) / 0.13f; return dx * dx + dy * dy; };
  if (head(x, y) <= 1.f) return 1.f;
  if (std::fabs(x - cx) < 0.045f && y > 0.44f && y < 0.62f) return 1.f;                    // 목
  { float dx = (x - cx) / 0.33f, dy = (y - 0.82f) / 0.24f; if (dx * dx + dy * dy <= 1.f || (y > 0.82f && std::fabs(x - cx) < 0.33f)) return 1.f; }
  if (!with_hair) return 0.f;
  // 머리카락 가닥: 머리 윗부분(y<0.40) 윤곽 밖 0~0.035H, 각도 방향 가는 선
  if (y < 0.40f) {
    float dx = (x - cx) / 0.10f, dy = (y - 0.36f) / 0.13f;
    float r = std::sqrt(dx * dx + dy * dy);
    float out = (r - 1.f) * 0.115f;  // ≈ 윤곽 밖 거리 (H 단위)
    if (out > 0 && out < 0.035f) {
      float th = std::atan2(dy, dx);
      float s = th * 90.f / (2 * kPi) + 0.3f * value_noise(out * 120.f, th * 3.f, 77);
      int idx = (int)std::floor(s);
      float frac = s - idx;
      float len = 0.012f + 0.023f * hash01(idx, 3, 99);
      if (out < len && std::fabs(frac - 0.5f) < 0.12f) return 1.f;
    }
  }
  return 0.f;
}

void SensorEmu::scene_rgb(float u, float v, int frame, float o[3]) const {
  const float H = (float)p_.height, A = (float)p_.width / p_.height;
  const float x = u / H, y = v / H;  // H 단위 (등방)

  if (p_.scene == Scene::kPortrait) {
    // 배경: 먼 책장(세로 띠) + 작은 광원들 (보케 원이 될 하이라이트)
    int band = (int)std::floor(x * 22.f);
    float hb = hash01(band, 0, 5);
    float lum = 0.10f + 0.22f * hb;
    float tex = 0.8f + 0.4f * fbm(x * 40.f, y * 8.f, 3, 11);
    set3(o, lum * (0.7f + 0.6f * hash01(band, 1, 5)) * tex, lum * (0.7f + 0.4f * hash01(band, 2, 5)) * tex,
         lum * (0.6f + 0.5f * hash01(band, 3, 5)) * tex);
    if (y > 0.72f) { float f = 0.55f; o[0] *= f; o[1] *= f; o[2] *= f; }   // 바닥/테이블
    for (int k = 0; k < 48; ++k) {
      float lx = hash01(k, 10, 21) * A, ly = 0.03f + 0.5f * hash01(k, 11, 21), lr = 0.004f + 0.006f * hash01(k, 12, 21);
      float dx = x - lx, dy = y - ly;
      if (dx * dx + dy * dy < lr * lr) {
        float b = 3.f + 5.f * hash01(k, 13, 21);
        set3(o, b, b * (0.72f + 0.1f * hash01(k, 14, 21)), b * 0.45f);
      }
    }
    // 인물
    if (person_alpha(u, v, frame, true) > 0.5f) {
      const float fx = x - (0.6f * std::sin(frame * 1.3f)) / H, fy = y - (0.4f * std::cos(frame * 0.9f)) / H;
      const float cx = 0.5f * A;
      float dx = (fx - cx) / 0.10f, dy = (fy - 0.36f) / 0.13f, r2 = dx * dx + dy * dy;
      if (person_alpha(u, v, frame, false) < 0.5f || (r2 <= 1.f && fy < 0.30f - 0.02f * dx * dx)) {
        float t = 0.03f + 0.02f * fbm(fx * 400.f, fy * 60.f, 2, 31);           // 머리카락 (가는 결)
        set3(o, t * 1.2f, t, t * 0.8f);
      } else if (r2 <= 1.f || fy < 0.62f) {
        float shade = 1.f - 0.35f * std::min(1.f, r2);
        float sk = shade * (0.9f + 0.1f * fbm(fx * 200.f, fy * 200.f, 2, 41));
        set3(o, 0.50f * sk, 0.33f * sk, 0.25f * sk);
        auto eye = [&](float ex) { float a = (fx - ex) / 0.014f, b = (fy - 0.345f) / 0.008f; return a * a + b * b < 1.f; };
        if (eye(cx - 0.036f) || eye(cx + 0.036f)) set3(o, 0.02f, 0.015f, 0.012f);
        if (std::fabs(fy - 0.425f) < 0.004f && std::fabs(fx - cx) < 0.03f) set3(o, 0.18f, 0.06f, 0.06f);  // 입
      } else {
        float st = 0.8f + 0.2f * (std::fmod(std::fabs(fx - cx) * 120.f, 2.f) < 1.f ? 1.f : 0.f);
        float tx = 0.85f + 0.3f * fbm(fx * 150.f, fy * 150.f, 3, 51);
        set3(o, 0.06f * st * tx, 0.12f * st * tx, 0.30f * st * tx);             // 셔츠
      }
    }
    return;
  }

  // ---- 정적 장면 (static / motion / lowlight 공통) ----
  float wall = (0.8f + 0.4f * x / A) * (0.9f + 0.2f * fbm(x * 6.f, y * 6.f, 4, 1));
  set3(o, 0.30f * wall, 0.27f * wall, 0.22f * wall);
  if (in_rect(x, y, 0.05f * A, 0.06f, 0.45f * A, 0.46f)) {                      // 고주파 직물 텍스처 (정렬 특징)
    float t = 0.15f + 0.5f * fbm(x * 300.f, y * 300.f, 3, 3);
    set3(o, 0.9f * t, 0.5f * t, 0.3f * t);
  }
  {                                                                             // 시멘스 스타 (해상력)
    float dx = x - 0.72f * A, dy = y - 0.27f;
    if (dx * dx + dy * dy < 0.17f * 0.17f) {
      float s = std::sin(36.f * std::atan2(dy, dx));
      float b = s > 0 ? 0.6f : 0.04f;
      set3(o, b, b, b);
    }
  }
  if (in_rect(x, y, 0.55f * A, 0.58f, 0.75f * A, 0.88f)) set3(o, 0.18f, 0.18f, 0.18f);  // 18% 회색 카드 (SNR 측정)
  if (in_rect(x, y, 0.05f * A, 0.55f, 0.45f * A, 0.92f)) {                      // 컬러 차트 6×4
    float cw = 0.40f * A / 6.f, ch = 0.37f / 4.f;
    int ci = (int)((x - 0.05f * A) / cw), cj = (int)((y - 0.55f) / ch);
    float fx = (x - 0.05f * A) / cw - ci, fy = (y - 0.55f) / ch - cj;
    if (fx < 0.1f || fx > 0.9f || fy < 0.1f || fy > 0.9f) set3(o, 0.01f, 0.01f, 0.01f);
    else { const int* c = kMacbeth[std::min(23, cj * 6 + std::min(ci, 5))]; set3(o, srgb8_to_lin(c[0]), srgb8_to_lin(c[1]), srgb8_to_lin(c[2])); }
  }
  {                                                                             // 조명 (클리핑되는 하이라이트)
    float dx = x - 0.93f * A, dy = y - 0.10f;
    if (dx * dx + dy * dy < 0.05f * 0.05f) set3(o, 6.f, 5.5f, 4.5f);
  }
  if (p_.scene == Scene::kMotion) {                                             // 흔드는 손: 프레임당 0.025H 이동
    float cx = 0.28f + 0.025f * frame, cy = 0.42f + 0.01f * std::sin(frame * 1.7f);
    float dx = (x - cx) / 0.07f, dy = (y - cy) / 0.09f;
    bool palm = dx * dx + dy * dy < 1.f;
    bool finger = false;
    for (int k = 0; k < 4; ++k) {
      float fx = cx - 0.045f + 0.03f * k;
      if (std::fabs(x - fx) < 0.011f && y > cy - 0.19f + 0.02f * std::fabs(k - 1.5f) && y < cy) finger = true;
    }
    if (palm || finger) {
      float sk = 0.85f + 0.15f * fbm(x * 150.f, y * 150.f, 2, 61);
      set3(o, 0.52f * sk, 0.34f * sk, 0.26f * sk);
    }
  }
}

void SensorEmu::render(int i, Image<uint16_t>& out, bool noise, ThreadPool& pool) const {
  const int W = p_.width, Hh = p_.height;
  const float sx = truth_.shift_x[i], sy = truth_.shift_y[i];
  const float black = meta_.black_level[0], range = meta_.white_level - black;
  const float* M = cam_from_srgb_;
  const float na = meta_.noise_a, nb = meta_.noise_b;
  pool.parallel_for(Hh, [&](int y) {
    Rng rng(((uint64_t)p_.seed << 40) ^ ((uint64_t)i << 24) ^ (uint64_t)y);
    uint16_t* row = out.row(y);
    for (int x = 0; x < W; ++x) {
      float s[3];
      scene_rgb(x + 0.5f - sx, y + 0.5f - sy, i, s);
      const int c = kCfaColor[p_.cfa][(y & 1) * 2 + (x & 1)];
      const int ch = c == 0 ? 0 : c == 3 ? 2 : 1;
      float cam = M[ch * 3 + 0] * s[0] + M[ch * 3 + 1] * s[1] + M[ch * 3 + 2] * s[2];
      float xn = std::max(0.f, cam / meta_.wb_gains[c] * expo_scale_);
      xn = std::min(xn, 1.0f);                                   // 광전 포화
      if (noise) xn += std::sqrt(na * xn + nb) * rng.gauss();    // Camera2 SENSOR_NOISE_PROFILE 모델
      float dn = black + xn * range;
      dn = std::min((float)meta_.white_level, std::max(0.f, std::nearbyint(dn)));
      row[x] = (uint16_t)dn;
    }
  });
}

void SensorEmu::render_rgb_truth(int i, float* out, ThreadPool& pool) const {
  const int W = p_.width, Hh = p_.height;
  const float sx = truth_.shift_x[i], sy = truth_.shift_y[i];
  const float* M = cam_from_srgb_;
  const int wbi[3] = {0, 1, 3};
  pool.parallel_for(Hh, [&](int y) {
    for (int x = 0; x < W; ++x) {
      float s[3];
      scene_rgb(x + 0.5f - sx, y + 0.5f - sy, i, s);
      for (int ch = 0; ch < 3; ++ch) {
        const float cam = M[ch * 3 + 0] * s[0] + M[ch * 3 + 1] * s[1] + M[ch * 3 + 2] * s[2];
        const float g = meta_.wb_gains[wbi[ch]];
        const float raw = std::min(1.f, std::max(0.f, cam / g * expo_scale_));
        out[((size_t)y * W + x) * 3 + ch] = std::min(1.f, raw * g);
      }
    }
  });
}

void SensorEmu::seg_mask256(float* out) const {
  std::fill(out, out + 256 * 256, 0.f);
  if (p_.scene != Scene::kPortrait) return;
  // 모델은 가는 머리카락 가닥을 놓친다 → 가닥 없는 실루엣의 커버리지 (가이디드 필터가 복원할 몫)
  std::vector<float> a(256 * 256);
  const float cw = (float)p_.width / 256, chh = (float)p_.height / 256;
  for (int j = 0; j < 256; ++j)
    for (int i = 0; i < 256; ++i) {
      float s = 0;
      for (int k = 0; k < 16; ++k) s += person_alpha((i + (k % 4 + 0.5f) / 4) * cw, (j + (k / 4 + 0.5f) / 4) * chh, 0, false);
      a[j * 256 + i] = s / 16;
    }
  // 모델 출력의 부드러운 경계: 3×3 박스 2회 (≈ 저해상도 디코더)
  std::vector<float> t(256 * 256);
  for (int pass = 0; pass < 2; ++pass) {
    for (int j = 0; j < 256; ++j)
      for (int i = 0; i < 256; ++i) {
        float s = 0; int n = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) {
            int jj = j + dj, ii = i + di;
            if (jj < 0 || jj > 255 || ii < 0 || ii > 255) continue;
            s += a[jj * 256 + ii]; ++n;
          }
        t[j * 256 + i] = s / n;
      }
    a.swap(t);
  }
  std::copy(a.begin(), a.end(), out);
}

void SensorEmu::alpha_gt(int w, int h, float* out) const {
  const float cw = (float)p_.width / w, chh = (float)p_.height / h;
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      if (p_.scene != Scene::kPortrait) { out[j * w + i] = 0; continue; }
      float s = 0;
      for (int k = 0; k < 4; ++k) s += person_alpha((i + (k % 2 + 0.5f) / 2) * cw, (j + (k / 2 + 0.5f) / 2) * chh, 0, true);
      out[j * w + i] = s / 4;
    }
}

bool write_burst(const std::string& dir, const EmuParams& p, bool write_clean, int threads, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  if (p.width % 4 || p.height % 4 || p.frames < 1 || p.frames > 16) return fail("width/height는 4의 배수, frames 1..16");
#if defined(_WIN32)
  _mkdir(dir.c_str());
#else
  mkdir(dir.c_str(), 0755);
#endif
  SensorEmu emu(p);
  ThreadPool pool(threads);
  Buffer<uint16_t> frame(p.width, p.height);
  char name[64];
  for (int i = 0; i < p.frames; ++i) {
    emu.render(i, frame.img, p.noise, pool);
    std::snprintf(name, sizeof name, "/frame_%02d.raw16", i);
    if (!write_raw16(dir + name, frame.img)) return fail("write " + dir + name);
    if (write_clean) {
      emu.render(i, frame.img, false, pool);
      std::snprintf(name, sizeof name, "/clean_%02d.raw16", i);
      if (!write_raw16(dir + name, frame.img)) return fail("write " + dir + name);
    }
  }
  { std::ofstream f(dir + "/meta.txt"); if (!f) return fail("write meta.txt"); f << format_meta(emu.meta()); }
  {
    const EmuTruth& t = emu.truth();
    std::ofstream f(dir + "/truth.txt");
    f << "emulated 1\nscene " << scene_name(p.scene) << "\nseed " << p.seed << "\nev_gain " << t.ev_gain << "\n";
    if (t.flat[2] > 0) f << "flat " << t.flat[0] << " " << t.flat[1] << " " << t.flat[2] << " " << t.flat[3] << "\n";
    if (t.moving[2] > 0) f << "moving " << t.moving[0] << " " << t.moving[1] << " " << t.moving[2] << " " << t.moving[3] << "\n";
    for (int i = 0; i < p.frames; ++i) f << "shift " << i << " " << t.shift_x[i] << " " << t.shift_y[i] << "\n";
  }
  if (p.scene == Scene::kPortrait) {
    std::vector<float> m(256 * 256);
    emu.seg_mask256(m.data());
    FILE* fp = std::fopen((dir + "/mask_emu.bin").c_str(), "wb");
    if (!fp) return fail("write mask_emu.bin");
    std::fwrite(m.data(), 4, m.size(), fp);
    std::fclose(fp);
    const int qw = p.width / 4, qh = p.height / 4;
    std::vector<float> a((size_t)qw * qh);
    emu.alpha_gt(qw, qh, a.data());
    fp = std::fopen((dir + "/alpha_gt_q.pgm").c_str(), "wb");
    if (!fp) return fail("write alpha_gt_q.pgm");
    std::fprintf(fp, "P5\n%d %d\n255\n", qw, qh);
    for (float v : a) std::fputc((int)(v * 255.f + 0.5f), fp);
    std::fclose(fp);
  }
  return true;
}

}  // namespace bp::emu
