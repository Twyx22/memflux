// bench_alloc : mesure RSS et débit de n'importe quel allocateur
// (système ou memflux-preload via LD_PRELOAD) sur charges typiques.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <unistd.h>
#include <vector>

static size_t rss_kb(){
  FILE* f = fopen("/proc/self/statm", "re");
  if(!f) return 0;
  unsigned long long tot = 0, rss = 0;
  int r = fscanf(f, "%llu %llu", &tot, &rss);
  fclose(f);
  (void)tot;
  return r == 2 ? rss * 4 : 0;
}

int main(int argc, char** argv){
  int nthreads = argc > 1 ? atoi(argv[1]) : 4;
  int iters    = argc > 2 ? atoi(argv[2]) : 200000;

  printf("bench_alloc: threads=%d iters=%d pid=%d\n", nthreads, iters, getpid());
  std::atomic<size_t> peak{0};
  auto t0 = std::chrono::steady_clock::now();

  auto worker = [&](int seed){
    std::mt19937 rng(seed);
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(4096);
    for(int i = 0; i < iters; ++i){
      size_t sz = 1u << (8 + (rng() % 12));      // 256 B .. 512 Kio
      if(rng() % 100 < 15) sz = 8 + (rng() % 4096);
      void* p = malloc(sz);
      if(p){ memset(p, (int)(rng() & 0xFF), sz < 64 ? sz : 64); live.push_back({p, sz}); }
      if(live.size() > 3000){
        // libère un bloc ancien aléatoire (pattern réaliste FIFO+random)
        size_t k = rng() % live.size();
        free(live[k].first);
        live[k] = live.back();
        live.pop_back();
      }
      if((i & 0xFFF) == 0){
        size_t r = rss_kb();
        size_t e = peak.load(std::memory_order_relaxed);
        while(r > e && !peak.compare_exchange_weak(e, r)) {}
      }
    }
    for(auto& [p, sz] : live) free(p);
  };

  std::vector<std::thread> ths;
  for(int i = 0; i < nthreads; ++i) ths.emplace_back(worker, i + 1);
  for(auto& t : ths) t.join();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count();
  printf("time=%lldms peak_rss=%zuMB final_rss=%zuMB\n",
         (long long)ms, peak.load() / 1024, rss_kb() / 1024);
  return 0;
}