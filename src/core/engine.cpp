#include "memflux/engine.hpp"
#include <algorithm>
#include <dirent.h>
#include <cstring>
#include <unistd.h>

namespace memflux {

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
    // ignore les processus du noyau (comm entre crochets)
    if(!s.comm.empty() && s.comm[0] == '[') continue;
    // ignore notre démon
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
    // majflt delta = activité récente → anti-cible
    if(s.majflt >= tr.prev_majflt) ps.reclaimed_recent = false;
    tr.prev_majflt = s.majflt;
    out.push_back(ps);
  }
  closedir(d);
  return out;
}

void Engine::compute_scores(){
  for(auto& ps : snapshot_){
    auto it = tracked_.find(ps.pid);
    if(it == tracked_.end()) continue;
    auto& tr = it->second;

    // ---- working set : échantillonnage pagemap (léger : seulement les
    // gros processus anon) -------------------------------------------
    if(ps.anon_kb > (cfg_.min_reclaimable_mb << 10) / 2){
      if(tr.samples == 0) clear_soft_dirty(ps.pid);  // début de fenêtre
      else {
        auto ws = sample_working_set(ps.pid);
        if(ws.ok && ws.anon_total_kb){
          ps.ws_prev_kb = tr.ws_touched_kb;
          tr.ws_touched_kb = ws.touched_kb;
          ps.ws_ratio = (double)ws.touched_kb / (double)std::max(ws.anon_total_kb, (uint64_t)1);
          ps.ws_prev_kb = ws.touched_kb;
          ps.anon_kb = std::max(ps.anon_kb, ws.anon_total_kb);
        }
      }
      ++tr.samples;
    } else {
      ps.ws_ratio = 1.0; // pas d'info → considère chaud
    }

    // ---- score = pression d'intérêt pour pageout -------------------
    // gros anon × working set froid + swap déjà présent (signal fort)
    double mb = (double)ps.anon_kb / 1024.0;
    double cold = 1.0 - std::min(1.0, ps.ws_ratio * 4.0); // ws<25% → froid
    double swap_press = ps.swap_kb ? 1.3 : 1.0;
    ps.score = mb * cold * swap_press;
    if(tr.cooldown > 0){ --tr.cooldown; ps.score = 0; }
  }
}

uint64_t Engine::heap_trim(const ProcessStat& ps, kern::PidFd& fd){
  if(!cfg_.enable_heap_trim || cfg_.dry_run) return 0;
  // Trim des grands segments anon : process_madvise(MADV_PAGEOUT).
  auto vmas = read_maps(ps.pid);
  uint64_t freed = 0;
  const uint64_t min_region = 8u << 20; // 8 Mo
  for(auto& v : vmas){
    if(!v.priv_anon || !v.readable) continue;
    uint64_t len = v.end - v.start;
    if(len < min_region) continue;
    ssize_t r = kern::pageout_pages(fd.fd, (void*)v.start, len, /*cold_only*/ false);
    if(r > 0) freed += (uint64_t)r;
  }
  return freed;
}

uint64_t Engine::act_on(const ProcessStat& ps, kern::PidFd& fd){
  uint64_t before = kern::read_proc_stat(ps.pid).anon_rss_kb;
  if(!before) return 0;
  uint64_t pageout_bytes = heap_trim(ps, fd);
  uint64_t after = kern::read_proc_stat(ps.pid).anon_rss_kb;
  uint64_t delta = before > after ? before - after : 0;
  // process_madvise retourne les octets traités : mesure directe fiable
  return pageout_bytes ? pageout_bytes : delta;
}

uint64_t Engine::run_cycle(){
  ++g_stats.cycles;
  snapshot_ = scan_processes();

  // ---- gate : PSI mémoire OU disponibilité critique -----------------
  auto psi = kern::psi_memory();
  bool pressure = psi.ok && psi.some_avg10 >= cfg_.psi_threshold;

  // mesure working set à chaque cycle (fenêtre = interval_ms)
  compute_scores();

  // 2e déclencheur : MemAvailable bas (la pression PSI ne monte que
  // lorsque le noyau est déjà en train de récupérer de la mémoire)
  auto mem = kern::meminfo();
  if(mem.total_kb && mem.available_kb){
    double avail_ratio = (double)mem.available_kb / (double)mem.total_kb;
    if(avail_ratio < 0.10) pressure = true;                 // < 10 % dispo
    else if(avail_ratio < 0.15 && psi.some_avg10 >= cfg_.psi_low) pressure = true;
  }

  if(!pressure && !cfg_.dry_run){
    // mode calme : on laisse le kernel respirer, on échantillonne seulement
    return 0;
  }
  return run_actions(mem);
}

uint64_t Engine::force_cycle(){
  ++g_stats.cycles;
  snapshot_ = scan_processes();
  compute_scores();
  auto mem = kern::meminfo();
  return run_actions(mem);
}

uint64_t Engine::run_actions(const kern::MemInfo& mem){
  // ---- sélection des cibles ---------------------------------------
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
    uint64_t got = act_on(*ps, fd);
    close(fd.fd);
    if(got){
      reclaimed_total += got;
      ++acted;
      auto& tr = tracked_[ps->pid];
      tr.cooldown = 5;
      LOG_INFO("pageout pid=", ps->pid, " comm=", ps->comm,
               " anon=", ps->anon_kb / 1024, "MB freed=", got / 1024, "MB",
               " ws_ratio=", ps->ws_ratio, " score=", ps->score);
    }
  }

  // ---- cgroup reclaim si toujours en pression ---------------------
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