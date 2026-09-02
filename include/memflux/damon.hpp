#pragma once
// Backend DAMON : pilote un kdamond noyau dédié (ops=vaddr) qui surveille
// les gros processus et expose les régions triées par température d'accès.
// Les régions dormantes retournées par schemes/0/tried_regions alimentent
// directement le moteur de décision — sans lire une seule page de pagemap.
#include "memflux/common.hpp"
#include <map>
#include <string>
#include <vector>

namespace memflux::damon {

struct Region {
  uint64_t start, end;      // bytes
  uint32_t nr_accesses;     // 0..max_nr_accesses (fenêtre aggr)
  uint32_t age;             // aggr intervals depuis dernier accès
};

struct TargetRegions {
  pid_t pid;
  std::vector<Region> regions;
};

// Availability & lifecycle ----------------------------------------------
bool available();            // damon_reclaim module présent ?
bool enabled();
bool enable(bool on);        // démarre / arrête le kdamond du module

// Configuration ----------------------------------------------------------
struct DamonConfig {
  uint32_t sample_us = 5000;      // échantillon page-level
  uint32_t aggr_us   = 100000;    // fenêtre d'agrégation (100 ms)
  uint32_t update_us = 1000000;   // refresh régions (1 s)
  uint32_t min_regions = 10;
  uint32_t max_regions = 1000;
  uint32_t refresh_ms = 1000;
  uint32_t min_age_ms = 10000;    // âge min d'une région froide (10 s)
  uint32_t quota_ms = 10;         // budget CPU par fenêtre
  uint32_t quota_reset_ms = 1000;
};
void configure(const DamonConfig& cfg);

// Cibles -----------------------------------------------------------------
// damon_reclaim surveille la mémoire physique globale — pas de cibles PID.
bool set_targets(const std::vector<pid_t>& pids);

// Lecture ----------------------------------------------------------------
std::map<pid_t, std::vector<Region>> snapshot();

// Stats noyau ------------------------------------------------------------
struct KernelStats {
  uint64_t nr_reclaimed = 0;      // régions réclamées depuis boot
  uint64_t bytes_reclaimed = 0;
  uint64_t nr_tried = 0;
  uint64_t bytes_tried = 0;
  int32_t  kdamond_pid = -1;
};
KernelStats kernel_stats();

// Stats internes
struct DamonStats {
  uint64_t snapshots = 0;
  uint64_t regions_total = 0;
  uint64_t errors = 0;
};
const DamonConfig& config();
DamonStats stats();

} // namespace memflux::damon