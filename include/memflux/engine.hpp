#pragma once
#include "memflux/common.hpp"
#include "memflux/kern.hpp"
#include "memflux/workingset.hpp"
#include <unordered_map>

namespace memflux {

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
  };

  Config& cfg_;
  std::unordered_map<pid_t, Track> tracked_;
  std::vector<ProcessStat> snapshot_;
  uint64_t psi_prev_ = 0;

  std::vector<ProcessStat> scan_processes();
  void compute_scores();
  uint64_t run_actions(const kern::MemInfo& mem);
  uint64_t act_on(const ProcessStat& ps, kern::PidFd& fd);
  uint64_t heap_trim(const ProcessStat& ps, kern::PidFd& fd);
};

} // namespace memflux