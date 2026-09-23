#pragma once
// 센서 에뮬레이터: 기기 없이 "Galaxy C55 후면 메인 RAW 버스트 덤프"와 같은 형식·통계를 만든다.
//   - 해상도 4080×3060 (50MP Quad-Bayer의 2×2 비닝 RAW16 가정), 10bit, 블랙 64, RGGB
//   - 장면 선형 sRGB → CCM⁻¹ → WB 게인⁻¹ → EV −1.5 노출 → Bayer 샘플 → 노이즈 분산 a·x+b → 양자화
//   - 프레임별 손떨림(서브픽셀 이동), 움직이는 물체, 저조도, 인물(+GT 마스크)
// core/는 이 모듈을 모른다. 실제 덤프가 생기면 bursts/ 의 에뮬 세트를 그대로 교체하면 된다.
#include <cstdint>
#include <string>
#include <vector>
#include "burstpipe/burst.h"
#include "burstpipe/thread_pool.h"

namespace bp::emu {

enum class Scene { kStatic, kMotion, kLowLight, kPortrait };
bool parse_scene(const std::string& s, Scene& out);
const char* scene_name(Scene s);

struct EmuParams {
  Scene scene = Scene::kStatic;
  int width = 4080, height = 3060;  // 4의 배수
  int frames = 8;
  int cfa = 0;                      // Camera2 enum: RGGB=0 GRBG=1 GBRG=2 BGGR=3
  uint32_t seed = 1;
  float shake_px = 2.5f;            // 손떨림 랜덤워크 표준편차 (raw px/프레임). 0이면 삼각대
  int iso = 0;                      // 0이면 장면 기본값
  bool noise = true;
};

struct EmuTruth {
  int flat[4] = {0, 0, 0, 0};       // 평탄 회색 카드 내부 (x,y,w,h raw 좌표, 흔들림 여유 포함). 없으면 0
  int moving[4] = {0, 0, 0, 0};     // 움직이는 물체가 지나간 영역. 없으면 0
  float ev_gain = 2.8f;             // 이 장면을 보기 좋게 만드는 finish ev_gain 권장값
  std::vector<float> shift_x, shift_y;  // 프레임별 카메라 이동 (raw px). image_i(x) = scene(x - shift_i)
};

class SensorEmu {
 public:
  explicit SensorEmu(const EmuParams& p);
  const EmuParams& params() const { return p_; }
  const BurstMeta& meta() const { return meta_; }
  const EmuTruth& truth() const { return truth_; }
  // frame i → out (width×height RAW16). noise=false면 노이즈 없는 기대값 (SNR/PSNR 기준 영상)
  void render(int i, Image<uint16_t>& out, bool noise, ThreadPool& pool) const;
  // 모자이크 전 정답: 픽셀마다 WB 적용 후 정규화 선형 RGB (finish의 lin과 같은 규약: min(1, raw)·wb, 1에서 클립). out: W*H*3
  void render_rgb_truth(int i, float* out, ThreadPool& pool) const;
  // 세그 모델 출력 흉내: 256×256, frame 0 기준 인물 실루엣(머리카락 가닥 제외) 커버리지를 블러. 인물 장면 외엔 0
  void seg_mask256(float* out) const;
  // GT 알파 (머리카락 포함), (w,h) 해상도로 frame 0 기준 커버리지
  void alpha_gt(int w, int h, float* out) const;

 private:
  void scene_rgb(float u, float v, int frame, float rgb[3]) const;  // u,v: raw px (프레임 0 카메라 기준)
  float person_alpha(float u, float v, int frame, bool with_hair) const;
  EmuParams p_;
  BurstMeta meta_;
  EmuTruth truth_;
  float cam_from_srgb_[9];   // CCM⁻¹
  float expo_scale_;         // 장면 1.0 → 정규화 raw
};

// dir/ 에 frame_NN.raw16, meta.txt, truth.txt (+인물: mask_emu.bin, alpha_gt_q.pgm) 를 쓴다.
// write_clean: clean_NN.raw16 (노이즈 없는 기대값)도 쓴다.
bool write_burst(const std::string& dir, const EmuParams& p, bool write_clean, int threads, std::string* err);

}  // namespace bp::emu
