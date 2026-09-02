// memhog : simulation d'un processus avec grosse empreinte anon et
// phases d'inactivité — sert à démontrer le pageout de memfluxd.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv){
  size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 512;
  int phase_s = argc > 2 ? atoi(argv[2]) : 30;
  printf("memhog pid=%d: %zu Mo, phases %ds\n", getpid(), mb, phase_s);
  std::vector<char*> keep;
  size_t chunk = 8u << 20;
  for(size_t got = 0; got < mb << 20; got += chunk){
    char* p = (char*)malloc(chunk);
    if(!p){ perror("malloc"); return 1; }
    memset(p, 0x5a, chunk);
    keep.push_back(p);
  }
  printf("alloc done, rss ~%zu Mo. Touch all / sleep alterné...\n", mb);
  for(int i = 0; i < 100; ++i){
    if(i % 2 == 0){
      // phase active : touche tout
      for(auto* p : keep) for(size_t o = 0; o < (mb << 20) && o < chunk; o += 4096)
        ((volatile char*)p)[o] = (char)i;
    }
    fflush(stdout);
    printf("tick %d\n", i), fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(phase_s / 10));
  }
  return 0;
}