#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>
#include "burstpipe/thread_pool.h"

// 짧은 parallel_for를 연속으로 많이 호출: 늦게 깬 워커가 이전 fn으로 다음 호출의 인덱스를 소비하면 합이 틀어진다.
int main() {
  bp::ThreadPool pool(4);
  long long total = 0;
  for (int rep = 0; rep < 20000; ++rep) {
    const int n = 1 + rep % 7;
    std::vector<int> hit(n, 0);
    std::atomic<int> sum{0};
    pool.parallel_for(n, [&](int i) { hit[i]++; sum += i + rep; });
    for (int i = 0; i < n; ++i) assert(hit[i] == 1);
    assert(sum == n * rep + n * (n - 1) / 2);
    total += sum;
  }
  std::printf("20000 generations ok (checksum %lld)\n", total);
  std::puts("test_threadpool OK");
  return 0;
}
