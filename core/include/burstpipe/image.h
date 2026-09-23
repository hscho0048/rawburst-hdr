#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

namespace bp {

template <typename T>
struct Image {
  int w = 0, h = 0;
  int stride = 0;      // 원소 단위
  T* data = nullptr;   // 비소유
  T* row(int y) { return data + (size_t)y * stride; }
  const T* row(int y) const { return data + (size_t)y * stride; }
  T& at(int x, int y) { return row(y)[x]; }
  T at(int x, int y) const { return row(y)[x]; }
  bool empty() const { return data == nullptr; }
};

// 작은 임시 버퍼용 (테스트, 마스크 등). 큰 프레임은 Arena.
template <typename T>
struct Buffer {
  std::vector<T> v;
  Image<T> img;
  Buffer() = default;
  Buffer(int w, int h) { resize(w, h); }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& o) noexcept : v(std::move(o.v)), img(o.img) { img.data = v.data(); }
  void resize(int w, int h) { v.assign((size_t)w * h, T{}); img = Image<T>{w, h, w, v.data()}; }
};

// 범프 할당기. free 없음. 시작 시 1회 크기 고정.
class Arena {
 public:
  explicit Arena(size_t bytes) : buf_(new uint8_t[bytes]), cap_(bytes) {}
  template <typename T>
  Image<T> alloc(int w, int h) {
    off_ = (off_ + 63) & ~size_t(63);
    size_t need = (size_t)w * h * sizeof(T);
    if (off_ + need > cap_) std::abort();  // 넘치면 설계 오류. 크기는 Pipeline이 계산
    Image<T> im{w, h, w, reinterpret_cast<T*>(buf_.get() + off_)};
    off_ += need;
    if (off_ > peak_) peak_ = off_;
    return im;
  }
  void reset() { off_ = 0; }
  size_t used() const { return off_; }
  size_t peak() const { return peak_; }
  size_t capacity() const { return cap_; }
 private:
  std::unique_ptr<uint8_t[]> buf_;
  size_t cap_, off_ = 0, peak_ = 0;
};

}  // namespace bp
