#include "memflux/workingset.hpp"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace memflux {

std::vector<VmaRange> read_maps(pid_t pid){
  std::vector<VmaRange> out;
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/maps", pid);
  FILE* f = fopen(path, "re");
  if(!f) return out;
  char line[512];
  while(fgets(line, sizeof line, f)){
    unsigned long a = 0, b = 0;
    char perms[8] = {0};
    if(sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
    VmaRange r;
    r.start = a; r.end = b;
    r.readable = perms[0] == 'r';
    r.writable = perms[1] == 'w';
    // anon privé : pas de chemin fichier sur la ligne
    r.priv_anon = r.writable && !strchr(line, '/');
    out.push_back(r);
  }
  fclose(f);
  return out;
}

bool clear_soft_dirty(pid_t pid){
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/clear_refs", pid);
  FILE* f = fopen(path, "we");
  if(!f) return false;
  bool ok = fwrite("4\n", 2, 1, f) == 1;
  fclose(f);
  return ok;
}

WsSample sample_working_set(pid_t pid){
  WsSample s;
  int fd = -1;
  {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/pagemap", pid);
    fd = open(path, O_RDONLY);
    if(fd < 0) return s;
  }
  auto vmas = read_maps(pid);
  uint64_t touched = 0, total = 0;
  std::vector<uint64_t> buf;
  for(auto& v : vmas){
    if(!v.priv_anon || !v.readable) continue;
    uint64_t npages = (v.end - v.start) / 4096;
    if(npages == 0) continue;
    total += npages;
    // lis par chunks de 4096 entrées pagemap (32 ko)
    uint64_t off = (v.start / 4096) * 8;
    uint64_t remaining = npages;
    if(lseek(fd, (off_t)off, SEEK_SET) < 0) continue;
    buf.resize(4096);
    while(remaining){
      uint64_t want = remaining < 4096 ? remaining : 4096;
      ssize_t got = pread(fd, buf.data(), want * 8, (off_t)off);
      if(got <= 0) break;
      uint64_t n = (uint64_t)got / 8;
      for(uint64_t i = 0; i < n; ++i){
        uint64_t e = buf[i];
        // bit 55 = soft-dirty, bit 63 = present
        if(e & (1ULL << 55)) touched += 1;
      }
      remaining -= n;
      off += got;
    }
  }
  close(fd);
  s.anon_total_kb = total * 4;
  s.touched_kb = touched * 4;
  s.regions = vmas.size();
  s.ok = true;
  return s;
}

} // namespace memflux