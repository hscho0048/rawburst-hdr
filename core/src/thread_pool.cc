#include "burstpipe/thread_pool.h"
#include <algorithm>
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace {
int32_t current_tid() {
#if defined(__linux__)
  return (int32_t)gettid();
#else
  return 0;
#endif
}
}  // namespace

namespace bp {

void pin_current_thread(int cpu) {
#if defined(__linux__)
  if (cpu < 0) return;
  cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
#else
  (void)cpu;
#endif
}

ThreadPool::ThreadPool(int threads, const std::vector<int>& cpus) {
  tids_.assign(std::max(1, threads), 0);
  tids_[0] = current_tid();
  nworkers_ = std::max(0, threads - 1);  // 워커가 읽는 기대 개수는 스레드 생성 전에 고정 (workers_는 생성 중 변한다)
  for (int i = 0; i < threads - 1; ++i) {
    int cpu = cpus.empty() ? -1 : cpus[(i + 1) % cpus.size()];
    workers_.emplace_back(&ThreadPool::worker, this, i + 1, cpu);
  }
  if (!cpus.empty()) pin_current_thread(cpus[0]);
  std::unique_lock<std::mutex> lk(m_);  // 모든 워커가 tid를 기록할 때까지
  done_cv_.wait(lk, [&] { return registered_ == nworkers_; });
}

ThreadPool::~ThreadPool() {
  { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
  cv_.notify_all();
  for (auto& t : workers_) t.join();
}

void ThreadPool::worker(int idx, int cpu) {
  pin_current_thread(cpu);
  {
    std::lock_guard<std::mutex> lk(m_);
    tids_[idx] = current_tid();
    if (++registered_ == nworkers_) done_cv_.notify_all();
  }
  int seen = 0;
  for (;;) {
    const std::function<void(int)>* fn; int n;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
      if (stop_) return;
      seen = generation_; fn = fn_; n = n_;
    }
    for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) (*fn)(i);
    {
      std::lock_guard<std::mutex> lk(m_);
      if (++finished_ == (int)workers_.size()) done_cv_.notify_one();
    }
  }
}

void ThreadPool::parallel_for(int n, const std::function<void(int)>& fn) {
  if (n <= 0) return;
  if (workers_.empty() || n == 1) { for (int i = 0; i < n; ++i) fn(i); return; }
  {
    std::lock_guard<std::mutex> lk(m_);
    fn_ = &fn; n_ = n; next_ = 0; finished_ = 0; ++generation_;
  }
  cv_.notify_all();
  for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) fn(i);
  // 모든 워커가 이번 generation을 소비할 때까지 대기 → fn 수명과 next_ 재설정이 안전
  std::unique_lock<std::mutex> lk(m_);
  done_cv_.wait(lk, [&] { return finished_ == (int)workers_.size(); });
}

}  // namespace bp
