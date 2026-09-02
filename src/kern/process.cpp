#include "memflux/kern.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MADV_COLD
#define MADV_COLD 20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

namespace memflux::kern {

PidFd pidfd_open(pid_t pid){
  PidFd r;
#ifdef SYS_pidfd_open
  r.fd = (int)syscall(SYS_pidfd_open, pid, 0);
#else
  errno = ENOSYS;
#endif
  return r;
}

ssize_t pageout_pages(int pidfd, void* addr, size_t len, bool cold_only){
  if(pidfd < 0) return -1;
  struct iovec iov{ addr, len };
#ifdef SYS_process_madvise
  int advice = cold_only ? MADV_COLD : MADV_PAGEOUT;
  return syscall(SYS_process_madvise, pidfd, &iov, 1, advice, 0);
#else
  (void)advice;
  errno = ENOSYS;
  return -1;
#endif
}

// ---------------------------------------------------------------- PSI
PsiSnapshot psi_memory(){
  PsiSnapshot s;
  FILE* f = fopen("/proc/pressure/memory", "re");
  if(!f) return s;
  char line[256];
  while(fgets(line, sizeof line, f)){
    double v10 = 0;
    if(sscanf(line, "some avg10=%lf", &v10) == 1){ s.some_avg10 = v10 / 100.0; s.ok = true; }
    else if(sscanf(line, "full avg10=%lf", &v10) == 1){ s.full_avg10 = v10 / 100.0; s.ok = true; }
  }
  fclose(f);
  return s;
}

// ---------------------------------------------------------------- meminfo
MemInfo meminfo(){
  MemInfo m;
  FILE* f = fopen("/proc/meminfo", "re");
  if(!f) return m;
  char key[64];
  unsigned long long val = 0;
  char unit[16];
  while(fscanf(f, "%63s %llu %15s", key, &val, unit) == 3){
    if(!strcmp(key, "MemTotal:")) m.total_kb = val;
    else if(!strcmp(key, "MemFree:")) m.free_kb = val;
    else if(!strcmp(key, "MemAvailable:")) m.available_kb = val;
    else if(!strcmp(key, "Anonymous:")) m.anon_kb = val;
    else if(!strcmp(key, "Mapped:")) m.file_kb = val;
    else if(!strcmp(key, "Slab:")) m.slab_kb = val;
    else if(!strcmp(key, "Shmem:")) m.shmem_kb = val;
    else if(!strcmp(key, "SwapTotal:")) m.swap_total_kb = val;
    else if(!strcmp(key, "SwapFree:")) m.swap_free_kb = val;
    if(fgetc(f) == EOF) break;
  }
  fclose(f);
  return m;
}

// ---------------------------------------------------------------- per-proc
ProcStat read_proc_stat(pid_t pid, uint64_t* prev_majflt){
  ProcStat s;
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  FILE* f = fopen(path, "re");
  if(!f) return s;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char* st = strrchr(buf, ')');
  if(!st) return s;
  ++st;
  int fields[64];
  long long nums[64];
  (void)fields;
  int idx = 0; // après comm (field 2)
  char* p = st;
  while(*p && idx < 64){
    while(*p == ' ') ++p;
    if(!*p) break;
    nums[idx++] = strtoll(p, &p, 10);
  }
  // nums[i] == field (3+i)
  if(idx < 24) return s;
  s.minflt = (uint64_t)nums[7];    // field 10
  s.majflt = (uint64_t)nums[9];    // field 12
  uint64_t maj = s.majflt;
  if(prev_majflt && maj >= *prev_majflt) s.majflt = maj - *prev_majflt;
  if(prev_majflt) *prev_majflt = maj;
  s.vss_kb = (uint64_t)nums[20] >> 10;   // field 23 vsize(bytes)
  s.rss_kb = (uint64_t)nums[21] << 2;    // field 24, pages
  s.ok = true;

  snprintf(path, sizeof path, "/proc/%d/status", pid);
  f = fopen(path, "re");
  if(f){
    char line[256];
    while(fgets(line, sizeof line, f)){
      if(!strncmp(line, "VmRSS:", 6)) sscanf(line + 6, "%llu", (unsigned long long*)&s.rss_kb);
      else if(!strncmp(line, "RssAnon:", 8)) sscanf(line + 8, "%llu", (unsigned long long*)&s.anon_rss_kb);
      else if(!strncmp(line, "RssFile:", 8)) sscanf(line + 8, "%llu", (unsigned long long*)&s.file_rss_kb);
      else if(!strncmp(line, "VmSwap:", 7)) sscanf(line + 7, "%llu", (unsigned long long*)&s.swap_kb);
      else if(!strncmp(line, "Name:", 5)){
        char* nm = line + 5;
        while(*nm == ' ' || *nm == '\t') ++nm;
        char* e = strchr(nm, '\n');
        if(e) *e = 0;
        s.comm = nm;
      }
    }
    fclose(f);
  }
  snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
  f = fopen(path, "re");
  if(f){
    char line[512];
    if(fgets(line, sizeof line, f)){
      char* colon = strrchr(line, ':');
      if(colon){
        char* e = strchr(++colon, '\n');
        if(e) *e = 0;
        s.cgroup = colon;
      }
    }
    fclose(f);
  }
  return s;
}

} // namespace memflux::kern