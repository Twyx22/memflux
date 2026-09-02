#pragma once
#include "memflux/common.hpp"
#include "memflux/kern.hpp"
#include "memflux/workingset.hpp"
#include "memflux/damon.hpp"
#include <unordered_map>

namespace memflux {

// Mode d'action de réclamation
enum class ReclaimMode : int {
  Pageout = 0,   // process_madvise(MADV_PAGEOUT) : froid + swap (défaut)
  ColdOnly = 1,  // process_madvise(MADV_COLD) : marque LRU-tail sans swap
                 // (charges latence-critiques : le noyau ne réclame qu'en
                 // vraie pression, pas de surcoût de latence en avance)
};

// Politique par cgroup (défini dans common.hpp, reflété ici)
using GroupPolicy = memflux::GroupPolicy;

// Moteur d'optimisation : sélectionne les processus cibles selon un score
// (RSS anon × ratio working-set) et applique les actions.
class Engine {
public:
  Engine(Config& cfg) : cfg_(cfg) {}

  // Un cycle complet : scan → score → actions. Retourne Kb réclamés.
  uint64_t run_cycle();
  // Cycle forcé : ignore le gate PSI/availability (démo/admin).
  uint64_t force_cycle();

  std::vector<ProcessStat> last_snapshot() const;

private:
  struct Track {
    uint64_t prev_majflt = 0;
    uint64_t ws_touched_kb = 0;
    uint64_t prev_anon_kb = 0;
    int cooldown = 0; // cycles restants avant nouvelle action
    int samples = 0;
    uint32_t damon_age_max = 0;   // région la plus vieille vue par DAMON
    uint32_t damon_cold_kb = 0;   // Ko dormants selon DAMON
  };

  Config& cfg_;
  std::unordered_map<pid_t, Track> tracked_;
  std::vector<ProcessStat> snapshot_;
  uint64_t psi_prev_ = 0;

  // backend sampler actif
  bool damon_on_ = false;
  std::vector<pid_t> damon_targets_;
  bool ebpf_on_ = false;        // traceur eBPF page faults

  std::vector<ProcessStat> scan_processes();
  void compute_scores();
  void damon_sample();
  uint64_t run_actions(const kern::MemInfo& mem);
  uint64_t act_on(const ProcessStat& ps, kern::PidFd& fd, ReclaimMode mode);
  uint64_t heap_trim(const ProcessStat& ps, kern::PidFd& fd, ReclaimMode mode);
  ReclaimMode mode_for(const ProcessStat& ps) const;
};

} // namespace memflux