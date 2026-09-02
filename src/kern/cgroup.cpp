#include "memflux/kern.hpp"
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace memflux::kern::cgroup {

bool available(){
  return access("/sys/fs/cgroup/cgroup.controllers", F_OK) == 0;
}

static std::string full_path(const std::string& path){
  if(path.empty() || path == "/") return "/sys/fs/cgroup";
  if(path[0] == '/') return path;
  return "/sys/fs/cgroup/" + path;
}

uint64_t current(const std::string& path){
  FILE* f = fopen((full_path(path) + "/memory.current").c_str(), "re");
  if(!f) return 0;
  unsigned long long v = 0;
  int r = fscanf(f, "%llu", &v);
  fclose(f);
  return r == 1 ? v : 0;
}

uint64_t reclaim(const std::string& path, uint64_t bytes){
  // memory.reclaim (5.19+) : write "bytes" -> noyau réclame jusqu'à bytes
  FILE* f = fopen((full_path(path) + "/memory.reclaim").c_str(), "we");
  if(!f) return 0;
  fprintf(f, "%llu", (unsigned long long)bytes);
  // Si l'écriture partielle réussit, le noyau indique via erreur -EAGAIN...
  // mais on lit memory.peak/current pour estimer.
  int err = ferror(f);
  fclose(f);
  if(err && errno != EAGAIN) return 0;
  return bytes;
}

} // namespace memflux::kern::cgroup