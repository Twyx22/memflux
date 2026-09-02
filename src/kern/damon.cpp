#include "memflux/damon.hpp"
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace memflux::damon {

// ------------------------------------------------------------------
// Ce backend pilote les modules noyaux damon_reclaim / damon_lru_sort
// (CONFIG_DAMON_RECLAIM / CONFIG_DAMON_LRU_SORT) dont les paramètres sont
// exposés sous /sys/module/*/parameters/. C'est l'approche recommandée par
// la communauté (l'API sysfs "admin" ne snapshot pas les régions cibles de
// façon fiable sur tous les noyaux, alors que ces modules sont maintenus).
//
// Sémantique des watermarks DAMON (per-mille de MemTotal) :
//   free > high → monitoring EN PAUSE (système sain, ne rien faire)
//   free < low  → monitoring STOPPÉ aussi (urgence : le noyau reclaim seul)
//   low ≤ free ≤ high → monitoring ACTIF : réclamation proactive
// ----------------------------------------------------------------

static bool w_num(const char* path, uint64_t v){
  std::ofstream f(path);
  if(!f) return false;
  f << v;
  return !f.fail();
}
static bool w_str(const char* path, const std::string& v){
  std::ofstream f(path);
  if(!f) return false;
  f << v;
  return !f.fail();
}
static bool r_num(const char* path, uint64_t& v){
  std::ifstream f(path);
  if(!f) return false;
  return (bool)(f >> v);
}

DamonConfig g_cfg;
DamonStats g_stats;
bool g_enabled = false;

const DamonConfig& config(){ return g_cfg; }
DamonStats stats(){ return g_stats; }

bool available(){
  struct stat st;
  return stat("/sys/module/damon_reclaim/parameters/enabled", &st) == 0;
}

bool enabled(){
  uint64_t v = 0;
  return r_num("/sys/module/damon_reclaim/parameters/enabled", v) && v != 0;
}

void configure(const DamonConfig& cfg){
  g_cfg = cfg;
  if(!available()) return;
  // intervals & fenêtre de monitoring
  w_num("/sys/module/damon_reclaim/parameters/sample_interval", cfg.sample_us);
  w_num("/sys/module/damon_reclaim/parameters/aggr_interval",   cfg.aggr_us);
  w_num("/sys/module/damon_reclaim/parameters/min_nr_regions", 10);
  w_num("/sys/module/damon_reclaim/parameters/max_nr_regions",  cfg.max_regions);
  // min_age : âge (aggr_intervals × aggr_us) à partir duquel une région
  // froide est réclamable. cfg.min_age_ms en millisecondes.
  w_num("/sys/module/damon_reclaim/parameters/min_age", (uint64_t)cfg.min_age_ms * 1000);
  // quota : budget temps CPU par fenêtre
  w_num("/sys/module/damon_reclaim/parameters/quota_ms", cfg.quota_ms);
  w_num("/sys/module/damon_reclaim/parameters/quota_reset_interval_ms", cfg.quota_reset_ms);
}

bool enable(bool on){
  if(!available()) return false;
  if(on == enabled()) return true;
  return w_str("/sys/module/damon_reclaim/parameters/enabled", on ? "Y" : "N");
}

bool set_targets(const std::vector<pid_t>&){
  // damon_reclaim surveille la mémoire physique globale — pas de cibles PID
  return false;
}

std::map<pid_t, std::vector<Region>> snapshot(){
  // les régions tentées sont globales (physiques) : voir kernel_stats()
  return {};
}

KernelStats kernel_stats(){
  KernelStats s;
  auto num = [](const char* p, uint64_t& v){
    std::ifstream f(p);
    return (bool)(f >> v);
  };
  num("/sys/module/damon_reclaim/parameters/nr_reclaimed_regions", s.nr_reclaimed);
  num("/sys/module/damon_reclaim/parameters/bytes_reclaimed_regions", s.bytes_reclaimed);
  num("/sys/module/damon_reclaim/parameters/nr_reclaim_tried_regions", s.nr_tried);
  num("/sys/module/damon_reclaim/parameters/bytes_reclaim_tried_regions", s.bytes_tried);
  num("/sys/module/damon_reclaim/parameters/kdamond_pid", (uint64_t&)s.kdamond_pid);
  return s;
}

} // namespace memflux::damon