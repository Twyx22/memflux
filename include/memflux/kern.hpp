#pragma once
// Interface noyau de memflux : pidfd, process_madvise, PSI, pagemap,
// cgroup v2, sysctl/KSM. Toutes les primitives tolèrent l'absence de
// support (fonctions retournant false / -1) pour rester portable.
#include "memflux/common.hpp"
#include <sys/types.h>

namespace memflux::kern {

// ---- process handles (pidfd) ---------------------------------------------
struct PidFd {
  int fd = -1;
  explicit operator bool() const { return fd >= 0; }
};
PidFd pidfd_open(pid_t pid);

// ---- process_madvise ------------------------------------------------------
// MADV_COLD/MADV_PAGEOUT sur une plage du VMA d'un autre processus.
// Renvoie le nombre d'octets traités, -1 si non supporté/échec.
ssize_t pageout_pages(int pidfd, void* addr, size_t len, bool cold_only = false);

// ---- PSI ------------------------------------------------------------------
struct PsiSnapshot { double some_avg10 = 0; double full_avg10 = 0; bool ok = false; };
PsiSnapshot psi_memory();

// ---- meminfo --------------------------------------------------------------
struct MemInfo {
  uint64_t total_kb = 0, free_kb = 0, available_kb = 0;
  uint64_t anon_kb = 0, file_kb = 0, slab_kb = 0, shmem_kb = 0;
  uint64_t swap_total_kb = 0, swap_free_kb = 0;
  uint64_t pgmajfault_delta = 0;
};
MemInfo meminfo();

// ---- per-process ----------------------------------------------------------
struct ProcStat {
  uint64_t rss_kb = 0, anon_rss_kb = 0, file_rss_kb = 0, shmem_pss_kb = 0;
  uint64_t swap_kb = 0, vss_kb = 0;
  uint64_t minflt = 0, majflt = 0;
  std::string comm, cgroup;
  bool ok = false;
};
ProcStat read_proc_stat(pid_t pid, uint64_t* prev_majflt = nullptr);

// ---- cgroup v2 ------------------------------------------------------------
namespace cgroup {
bool available();
// Réclame `bytes` depuis le cgroup racine ou `path`. Renvoie octets réclamés.
uint64_t reclaim(const std::string& path, uint64_t bytes);
uint64_t current(const std::string& path);
} // namespace cgroup

// ---- KSM / sysctl ----------------------------------------------------------
namespace sysctl {
bool ksm_supported();
bool ksm_enable(bool on);
bool ksm_set(const char* name, const std::string& value);
std::string ksm_get(const char* name);
} // namespace sysctl

} // namespace memflux::kern