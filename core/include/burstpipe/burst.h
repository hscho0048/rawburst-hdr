#pragma once
#include <iosfwd>
#include <string>
#include <vector>
#include "burstpipe/image.h"

namespace bp {

// [cfa][(y&1)*2 + (x&1)] → 0=R 1=Gr 2=Gb 3=B
inline const int kCfaColor[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0}};

struct FrameMeta {
  int64_t timestamp_ns = 0, exposure_ns = 0;
  int iso = 0;
};

struct BurstMeta {
  int width = 0, height = 0, cfa = 0, white_level = 1023;
  float black_level[4] = {0, 0, 0, 0};       // CFA 위치 순서 (좌상, 우상, 좌하, 우하)
  float wb_gains[4] = {1, 1, 1, 1};          // R, G_even(Gr), G_odd(Gb), B
  float ccm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // 행 우선, sensor RGB → linear sRGB
  float noise_a = 0, noise_b = 0;            // 0,0이면 프로파일 없음
  int orientation = 0;                       // SENSOR_ORIENTATION: 센서 영상을 시계방향으로 이만큼 돌리면 정립 (0/90/180/270)
  std::vector<FrameMeta> frames;
  // CFA 위치 (x&1,y&1)의 블랙레벨과 WB 게인
  float black_at(int x, int y) const { return black_level[(y & 1) * 2 + (x & 1)]; }
  float gain_at(int x, int y) const { return wb_gains[kCfaColor[cfa][(y & 1) * 2 + (x & 1)]]; }
};

struct Burst {
  BurstMeta meta;
  std::vector<Image<uint16_t>> frames;  // 비소유. 메모리는 Arena 또는 JNI ByteBuffer
};

// dir/meta.txt + dir/frame_NN.raw16 을 arena로 읽는다. max_frames>0이면 앞에서 그만큼만.
bool load_burst(const std::string& dir, Arena& arena, Burst& out, int max_frames = 0);
bool load_meta(const std::string& path, BurstMeta& out);
bool parse_meta(std::istream& in, BurstMeta& out);       // meta.txt 본문 (JNI는 문자열로 받는다)
std::string format_meta(const BurstMeta& m);             // parse_meta의 역. 에뮬레이터·테스트용
// P5(16bit) / P6(8bit) 저장
bool write_pgm16(const std::string& path, const Image<uint16_t>& im);
bool write_ppm8(const std::string& path, const Image<uint8_t>& rgba);  // rgba: w*4 폭, alpha 버림
bool write_raw16(const std::string& path, const Image<uint16_t>& im);

}  // namespace bp
