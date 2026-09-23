#include "burstpipe/thread_pool.h"
#if defined(__linux__)
#include <sched.h>
#endif

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
  for (int i = 0; i < threads - 1; ++i) {
    int cpu = cpus.empty() ? -1 : cpus[(i + 1) % cpus.size()];
    workers_.emplace_back(&ThreadPool::worker, this, cpu);
  }
  if (!cpus.empty()) pin_current_thread(cpus[0]);
}

ThreadPool::~ThreadPool() {
  { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
  cv_.notify_all();
  for (auto& t : workers_) t.join();
}

void ThreadPool::worker(int cpu) {
  pin_current_thread(cpu);
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
