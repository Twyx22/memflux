#include "memflux/engine.hpp"
#include "memflux/ebpf.hpp"
#include <algorithm>
#include <dirent.h>
#include <cstring>
#include <unistd.h>

namespace memflux {

// ---------------------------------------------------------------- politiques
// Chargées depuis [groups] dans la conf (via Config::groups)
static ReclaimMode parse_mode(const std::string& s){
  std::string t = s;
  for(auto& c : t) c = (char)tolower(c);
  if(t == "cold" || t == "coldonly" || t == "cold_only") return ReclaimMode::ColdOnly;
  return ReclaimMode::Pageout;
}

ReclaimMode Engine::mode_for(const ProcessStat& ps) const{
  // politique par cgroup la plus longue qui matche
  const GroupPolicy* best = nullptr;
  for(const auto& g : cfg_.groups){
    if(ps.cgroup.rfind(g.cgroup_prefix, 0) == 0){
      if(!best || g.cgroup_prefix.size() > best->cgroup_prefix.size()) best = &g;
    }
  }
  if(best) return best->mode == 1 ? ReclaimMode::ColdOnly : ReclaimMode::Pageout;
  return cfg_.default_mode == "cold" ? ReclaimMode::ColdOnly : ReclaimMode::Pageout;
}

static double weight_for(const Config& cfg, const ProcessStat& ps){
  double w = 1.0;
  const GroupPolicy* best = nullptr;
  for(const auto& g : cfg.groups){
    if(ps.cgroup.rfind(g.cgroup_prefix, 0) == 0){
      if(!best || g.cgroup_prefix.size() > best->cgroup_prefix.size()) best = &g;
    }
  }
  if(best) w = best->weight;
  return w;
}

std::vector<ProcessStat> Engine::scan_processes(){
  std::vector<ProcessStat> out;
  DIR* d = opendir("/proc");
  if(!d) return out;
  struct dirent* e;
  while((e = readdir(d))){
    if(e->d_type != DT_DIR) continue;
    char* end = nullptr;
    long pid = strtol(e->d_name, &end, 10);
    if(!end || *end || pid <= 1) continue;
    pid_t p = (pid_t)pid;
    kern::ProcStat s = kern::read_proc_stat(p);
    if(!s.ok || s.rss_kb == 0) continue;
    if(!s.comm.empty() && s.comm[0] == '[') continue;
    if(p == getpid()) continue;
    ProcessStat ps;
    ps.pid = p;
    ps.comm = s.comm;
    ps.cgroup = s.cgroup;
    ps.rss_kb = s.rss_kb;
    ps.anon_kb = s.anon_rss_kb;
    ps.file_kb = s.file_rss_kb;
    ps.swap_kb = s.swap_kb;
    auto& tr = tracked_[p];
    if(s.majflt >= tr.prev_majflt) ps.reclaimed_recent = false;
    tr.prev_majflt = s.majflt;
    out.push_back(ps);
  }
  closedir(d);
  return out;
}

// ---------------------------------------------------------------- DAMON
void Engine::damon_sample(){
  // maintient la liste de cibles (gros anon) pour le kdamond
  std::vector<pid_t> targets;
  for(auto& ps : snapshot_){
    if(ps.anon_kb >= (cfg_.min_reclaimable_mb << 10) / 2) damon_targets_.push_back(ps.pid);
  }
  std::sort(damon_targets_.begin(), damon_targets_.end());
  damon_targets_.erase(std::unique(damon_targets_.begin(), damon_targets_.end()),
                       damon_targets_.end());

  if(damon_targets_.size() > 64) damon_targets_.resize(64);
  if(damon::set_targets(damon_targets_)){
    auto snap = damon::snapshot();
    for(auto& [pid, regs] : snap){
      auto it = tracked_.find(pid);
      if(it == tracked_.end()) continue;
      uint32_t cold_kb = 0, age_max = 0;
      for(const auto& r : regs){
        uint64_t kb = (r.end - r.start) >> 10;
        if(r.nr_accesses == 0){ cold_kb += (uint32_t)std::min<uint64_t>(kb, ~0u); }
        age_max = std::max(age_max, r.age);
      }
      it->second.damon_cold_kb = cold_kb;
      it->second.damon_age_max = age_max;
    }
    // marque la source pour l'affichage : ws_ratio reste le champ "froideur"
    for(auto& ps : snapshot_){
      auto it = tracked_.find(ps.pid);
      if(it == tracked_.end()) continue;
      if(ps.anon_kb > 0){
        double d = (double)std::min<uint64_t>(it->second.damon_cold_kb, ps.anon_kb)
                 / (double)ps.anon_kb;
        if(d > 0 && d <= 1.0) ps.ws_ratio = std::min(ps.ws_ratio, 1.0 - d);
      }
    }
  }
}

void Engine::compute_scores(){
  // backend DAMON si actif (sinon fallback pagemap soft-dirty)
  if(damon_on_) damon_sample();

  for(auto& ps : snapshot_){
    auto it = tracked_.find(ps.pid);
    if(it == tracked_.end()) continue;
    auto& tr = it->second;

    if(!damon_on_ && ps.anon_kb > (cfg_.min_reclaimable_mb << 10) / 2){
      // sampler pagemap soft-dirty (fallback universel)
      if(tr.samples == 0) clear_soft_dirty(ps.pid);
      else {
        auto ws = sample_working_set(ps.pid);
        if(ws.ok && ws.anon_total_kb){
          ps.ws_prev_kb = tr.ws_touched_kb;
          tr.ws_touched_kb = ws.touched_kb;
          ps.ws_ratio = (double)ws.touched_kb / (double)std::max(ws.anon_total_kb, (uint64_t)1);
          ps.anon_kb = std::max(ps.anon_kb, ws.anon_total_kb);
        }
      }
      ++tr.samples;
    } else if(!damon_on_){
      ps.ws_ratio = 1.0;
    }

    double mb = (double)ps.anon_kb / 1024.0;
    double cold = 1.0 - std::min(1.0, ps.ws_ratio * 4.0);
    double swap_press = ps.swap_kb ? 1.3 : 1.0;
    double w = weight_for(cfg_, ps);
    ps.score = mb * cold * swap_press * w;
    if(tr.cooldown > 0){ --tr.cooldown; ps.score = 0; }
  }
}

// ---------------------------------------------------------------- actions
uint64_t Engine::heap_trim(const ProcessStat& ps, kern::PidFd& fd, ReclaimMode mode){
  if(!cfg_.enable_heap_trim || cfg_.dry_run) return 0;
  auto vmas = read_maps(ps.pid);
  uint64_t freed = 0;
  const uint64_t min_region = 8u << 20;
  bool cold_only = mode == ReclaimMode::ColdOnly;
  for(auto& v : vmas){
    if(!v.priv_anon || !v.readable) continue;
    uint64_t len = v.end - v.start;
    if(len < min_region) continue;
    ssize_t r = kern::pageout_pages(fd.fd, (void*)v.start, len, cold_only);
    if(r > 0) freed += (uint64_t)r;
  }
  return freed;
}

uint64_t Engine::act_on(const ProcessStat& ps, kern::PidFd& fd, ReclaimMode mode){
  uint64_t before = kern::read_proc_stat(ps.pid).anon_rss_kb;
  if(!before) return 0;
  uint64_t bytes = heap_trim(ps, fd, mode);
  uint64_t after = kern::read_proc_stat(ps.pid).anon_rss_kb;
  uint64_t delta = before > after ? before - after : 0;
  return bytes ? bytes : delta;
}

uint64_t Engine::run_cycle(){
  ++g_stats.cycles;
  snapshot_ = scan_processes();

  // init backend DAMON au premier cycle si dispo & demandé
  if(!damon_on_ && cfg_.enable_damon && damon::available() && !damon::enabled()){
    damon::DamonConfig dc;
    dc.aggr_us = cfg_.interval_ms * 1000;
    dc.update_us = cfg_.interval_ms * 1000;
    dc.refresh_ms = cfg_.interval_ms;
    damon::configure(dc);
    damon_on_ = damon::enable(true);
    if(damon_on_){
      auto ks = damon::kernel_stats();
      LOG_INFO("damon_reclaim kdamond started (pid=", ks.kdamond_pid,
               " min_age=", damon::config().min_age_ms, "ms)");
    }
  }

  // init eBPF page-fault tracer (optionnel)
  if(!ebpf_on_ && ebpf::available()){
    ebpf_on_ = ebpf::load();
  }

  auto psi = kern::psi_memory();
  auto mem = kern::meminfo();

  // ---- garde-fou anti-OOM : en urgence mémoire, on n'ajoute RIEN ----
  // Crise = PSI full élevé (le noyau bloque sur du reclaim) ou
  // MemAvailable < 5 %. Dans ce cas memfluxd ne doit PAS agir (pageout = du
  // travail supplémentaire) : le noyau fait mieux, seul.
  bool crisis = false;
  if(psi.ok && psi.full_avg10 >= 0.25) crisis = true;
  double avail_ratio = 1.0;
  if(mem.total_kb && mem.available_kb){
    avail_ratio = (double)mem.available_kb / (double)mem.total_kb;
    if(avail_ratio < 0.05) crisis = true;
  }
  if(crisis){
    LOG_WARN("crisis guard: PSI_full=", psi.full_avg10, " avail=", avail_ratio,
             " → pausing all reclaim this cycle");
    return 0;
  }

  bool pressure = psi.ok && psi.some_avg10 >= cfg_.psi_threshold;
  if(mem.total_kb && mem.available_kb){
    if(avail_ratio < 0.10) pressure = true;
    else if(avail_ratio < 0.15 && psi.some_avg10 >= cfg_.psi_low) pressure = true;
  }

  // ---- eBPF : compteurs de page faults → protection anti-thrashing --
  // Un processus qui re-faute beaucoup après un pageout était trop chaud :
  // on le met en cooldown long (x4) et on log l'inefficacité.
  if(ebpf_on_ && ebpf::loaded()){
    auto faults = ebpf::drain_faults();
    for(auto& [pid, n] : faults){
      auto it = tracked_.find(pid);
      if(it == tracked_.end()) continue;
      // seuil : > 20 000 minor faults par cycle (fenêtre 1 s) = thrash
      if(n > 20000 && it->second.cooldown > 0){
        it->second.cooldown = std::max(it->second.cooldown, 20);
        LOG_WARN("thrash guard: pid=", pid, " re-faulted ", n,
                 " pages last cycle → cooldown 20 cycles");
      }
    }
  }

  compute_scores();

  if(!pressure && !cfg_.dry_run) return 0;
  return run_actions(mem);
}

uint64_t Engine::force_cycle(){
  ++g_stats.cycles;
  snapshot_ = scan_processes();
  if(!damon_on_ && cfg_.enable_damon && damon::available() && !damon::enabled()){
    damon::DamonConfig dc;
    damon::configure(dc);
    damon_on_ = damon::enable(true);
  }
  compute_scores();
  auto mem = kern::meminfo();
  return run_actions(mem);
}

uint64_t Engine::run_actions(const kern::MemInfo& mem){
  (void)mem;
  std::vector<ProcessStat*> cands;
  for(auto& ps : snapshot_){
    if(ps.score < cfg_.min_score) continue;
    if(ps.anon_kb < (cfg_.min_reclaimable_mb << 10)) continue;
    cands.push_back(&ps);
  }
  std::sort(cands.begin(), cands.end(),
            [](auto* a, auto* b){ return a->score > b->score; });

  uint64_t reclaimed_total = 0;
  uint32_t acted = 0;
  for(auto* ps : cands){
    if(acted >= cfg_.max_targets) break;
    if(reclaimed_total >= cfg_.max_reclaim_mb_per_cycle << 10) break;
    auto fd = kern::pidfd_open(ps->pid);
    if(!fd) { ++g_stats.errors; continue; }
    ReclaimMode mode = mode_for(*ps);
    uint64_t got = act_on(*ps, fd, mode);
    close(fd.fd);
    if(got){
      reclaimed_total += got;
      ++acted;
      tracked_[ps->pid].cooldown = 5;
      LOG_INFO((mode == ReclaimMode::ColdOnly ? "cold" : "pageout"),
               " pid=", ps->pid, " comm=", ps->comm,
               " anon=", ps->anon_kb / 1024, "MB freed=", got / 1024, "MB",
               " ws_ratio=", ps->ws_ratio, " score=", ps->score);
    }
  }

  // cgroup v2 memory.reclaim ciblé par politique (si pression persistante)
  auto psi2 = kern::psi_memory();
  if(cfg_.enable_cgroup_reclaim && psi2.ok && psi2.some_avg10 >= cfg_.psi_threshold * 1.5){
    if(kern::cgroup::available()){
      uint64_t want = (cfg_.max_reclaim_mb_per_cycle << 10) - reclaimed_total;
      if(want > (64u << 10)){
        uint64_t got = kern::cgroup::reclaim("/", want);
        if(got){
          g_stats.cgroup_reclaimed_kb += got / 1024;
          LOG_INFO("cgroup.reclaim want=", want / 1024, "MB");
        }
      }
    }
  }

  g_stats.pageouts_kb += reclaimed_total / 1024;
  return reclaimed_total / 1024;
}

std::vector<ProcessStat> Engine::last_snapshot() const { return snapshot_; }

} // namespace memflux