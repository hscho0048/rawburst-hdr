#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bp {

// 고정 크기 풀. parallel_for는 호출 스레드도 일한다. 중첩 호출 금지.
// 모든 워커가 매 generation에 체크인한 뒤에야 parallel_for가 반환한다 — 늦게 깬 워커가
// 다음 호출의 작업 카운터를 이전 fn으로 소비하는 레이스를 막기 위해서.
class ThreadPool {
 public:
  // cpus가 비어있지 않으면 호출 스레드는 cpus[0], 워커 i는 cpus[(i+1) % size]에 고정 (Linux/Android만)
  explicit ThreadPool(int threads, const std::vector<int>& cpus = {});
  ~ThreadPool();
  void parallel_for(int n, const std::function<void(int)>& fn);
  int size() const { return (int)workers_.size() + 1; }
 private:
  void worker(int cpu);
  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, done_cv_;
  const std::function<void(int)>* fn_ = nullptr;
  int n_ = 0, generation_ = 0, finished_ = 0;
  std::atomic<int> next_{0};
  bool stop_ = false;
};

void pin_current_thread(int cpu);  // 실패해도 무시

}  // namespace bp
