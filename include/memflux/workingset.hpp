#pragma once
// Working-set sampler façon DAMON : échantillonne la pagemap de régions
// (régions = VMAs arrondies à 2 Mo) pour estimer la fraction de pages
// réellement accédées récemment (bit soft-dirty) — via /proc/pid/clear_refs.
#include "memflux/common.hpp"
#include <vector>

namespace memflux {

struct VmaRange {
  uint64_t start, end;
  bool readable, writable, priv_anon;
};

std::vector<VmaRange> read_maps(pid_t pid);

struct WsSample {
  uint64_t anon_total_kb = 0;
  uint64_t touched_kb = 0;     // pages accédées depuis le dernier clear
  uint64_t regions = 0;
  bool ok = false;
};

// Marque le processus pour sampling (clear_refs = 4 → reset soft-dirty).
bool clear_soft_dirty(pid_t pid);
// Lit la pagemap et compte les pages soft-dirty dans les VMAs anon privées.
WsSample sample_working_set(pid_t pid);

} // namespace memflux