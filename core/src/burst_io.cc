#include "burstpipe/burst.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bp {

bool parse_meta(std::istream& f, BurstMeta& m) {
  std::string key;
  while (f >> key) {
    if (key == "width") f >> m.width;
    else if (key == "height") f >> m.height;
    else if (key == "cfa") f >> m.cfa;
    else if (key == "white_level") f >> m.white_level;
    else if (key == "black_level") for (float& v : m.black_level) f >> v;
    else if (key == "wb_gains") for (float& v : m.wb_gains) f >> v;
    else if (key == "ccm") for (float& v : m.ccm) f >> v;
    else if (key == "noise_profile") f >> m.noise_a >> m.noise_b;
    else if (key == "orientation") f >> m.orientation;
    else if (key == "frame") {
      int idx; FrameMeta fm;
      f >> idx >> fm.timestamp_ns >> fm.exposure_ns >> fm.iso;
      m.frames.push_back(fm);
    } else { std::string rest; std::getline(f, rest); }
  }
  if (m.cfa < 0 || m.cfa > 3) return false;
  return m.width > 0 && m.height > 0 && (m.width % 4) == 0 && (m.height % 4) == 0 && !m.frames.empty();
}

bool load_meta(const std::string& path, BurstMeta& m) {
  std::ifstream f(path);
  return f && parse_meta(f, m);
}

std::string format_meta(const BurstMeta& m) {
  std::ostringstream s;
  s.precision(7);
  s << "width " << m.width << "\nheight " << m.height << "\ncfa " << m.cfa << "\nwhite_level " << m.white_level << "\n";
  s << "black_level " << m.black_level[0] << " " << m.black_level[1] << " " << m.black_level[2] << " " << m.black_level[3] << "\n";
  s << "wb_gains " << m.wb_gains[0] << " " << m.wb_gains[1] << " " << m.wb_gains[2] << " " << m.wb_gains[3] << "\n";
  s << "ccm";
  for (float v : m.ccm) s << " " << v;
  s << "\n";
  if (m.noise_a > 0 || m.noise_b > 0) s << "noise_profile " << m.noise_a << " " << m.noise_b << "\n";
  if (m.orientation) s << "orientation " << m.orientation << "\n";
  for (size_t i = 0; i < m.frames.size(); ++i)
    s << "frame " << i << " " << m.frames[i].timestamp_ns << " " << m.frames[i].exposure_ns << " " << m.frames[i].iso << "\n";
  return s.str();
}

bool load_burst(const std::string& dir, Arena& arena, Burst& out, int max_frames) {
  out.meta = BurstMeta{};
  if (!load_meta(dir + "/meta.txt", out.meta)) return false;
  int n = (int)out.meta.frames.size();
  if (max_frames > 0 && max_frames < n) { n = max_frames; out.meta.frames.resize(n); }
  out.frames.clear();
  for (int i = 0; i < n; ++i) {
    char name[64]; std::snprintf(name, sizeof name, "/frame_%02d.raw16", i);
    FILE* fp = std::fopen((dir + name).c_str(), "rb");
    if (!fp) return false;
    Image<uint16_t> im = arena.alloc<uint16_t>(out.meta.width, out.meta.height);
    size_t want = (size_t)im.w * im.h;
    size_t got = std::fread(im.data, 2, want, fp);
    std::fclose(fp);
    if (got != want) return false;
    out.frames.push_back(im);
  }
  return true;
}

bool write_pgm16(const std::string& path, const Image<uint16_t>& im) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fprintf(fp, "P5\n%d %d\n65535\n", im.w, im.h);
  std::vector<uint8_t> row((size_t)im.w * 2);
  for (int y = 0; y < im.h; ++y) {  // PGM은 빅엔디언
    const uint16_t* r = im.row(y);
    for (int x = 0; x < im.w; ++x) { row[2 * x] = r[x] >> 8; row[2 * x + 1] = r[x] & 0xff; }
    std::fwrite(row.data(), 1, row.size(), fp);
  }
  std::fclose(fp);
  return true;
}

bool write_ppm8(const std::string& path, const Image<uint8_t>& rgba) {
  int w = rgba.w / 4;
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fprintf(fp, "P6\n%d %d\n255\n", w, rgba.h);
  std::vector<uint8_t> row((size_t)w * 3);
  for (int y = 0; y < rgba.h; ++y) {
    const uint8_t* r = rgba.row(y);
    for (int x = 0; x < w; ++x) { row[3 * x] = r[4 * x]; row[3 * x + 1] = r[4 * x + 1]; row[3 * x + 2] = r[4 * x + 2]; }
    std::fwrite(row.data(), 1, row.size(), fp);
  }
  std::fclose(fp);
  return true;
}

bool write_raw16(const std::string& path, const Image<uint16_t>& im) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  for (int y = 0; y < im.h; ++y) std::fwrite(im.row(y), 2, im.w, fp);
  std::fclose(fp);
  return true;
}

}  // namespace bp
