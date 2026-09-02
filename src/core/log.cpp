#include "memflux/common.hpp"
#include <ctime>
#include <fstream>
#include <mutex>
#include <sys/time.h>

namespace memflux {

namespace {
std::mutex g_log_mu;
LogLevel g_level = LogLevel::Info;
FILE* g_fp = nullptr;
bool g_own_fp = false;
} // namespace

void log_init(LogLevel lvl, const std::string& file){
  std::lock_guard lk(g_log_mu);
  g_level = lvl;
  if(g_fp && g_own_fp){ fclose(g_fp); g_fp = nullptr; g_own_fp = false; }
  if(!file.empty()){
    g_fp = fopen(file.c_str(), "a");
    g_own_fp = bool(g_fp);
  }
}

void log_set_level(LogLevel lvl){
  std::lock_guard lk(g_log_mu);
  g_level = lvl;
}

LogLevel log_level(){
  std::lock_guard lk(g_log_mu);
  return g_level;
}

bool log_enabled(LogLevel lvl){
  return lvl >= log_level();
}

void log_msg(LogLevel lvl, const std::string& msg){
  const char* tag = "IWE?D";
  (void)tag;
  static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  char ts[40];
  struct timespec tsr;
  clock_gettime(CLOCK_REALTIME, &tsr);
  struct tm tmv;
  localtime_r(&tsr.tv_sec, &tmv);
  strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tmv);
  std::string line = fmt_line("[", ts, ".", (unsigned long)(tsr.tv_nsec / 1000000), "] ",
                              names[(int)lvl], " ", msg, "\n");
  std::lock_guard lk(g_log_mu);
  if(g_fp){ fwrite(line.data(), 1, line.size(), g_fp); fflush(g_fp); }
  else { fwrite(line.data(), 1, line.size(), stderr); }
}

// ---------------------------------------------------------------- config
namespace {
std::string trim(const std::string& s){
  size_t a = s.find_first_not_of(" \t\r\n");
  if(a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
bool parse_bool(const std::string& v, bool dflt){
  std::string t = trim(v);
  for(auto& c : t) c = (char)tolower(c);
  if(t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if(t == "0" || t == "false" || t == "no" || t == "off") return false;
  return dflt;
}
uint64_t parse_u64(const std::string& v, uint64_t dflt){
  try { return std::stoull(trim(v)); } catch(...){ return dflt; }
}
double parse_dbl(const std::string& v, double dflt){
  try { return std::stod(trim(v)); } catch(...){ return dflt; }
}
} // namespace

Config Config::load(const std::string& path){
  Config c;
  c.reload(path);
  return c;
}

void Config::reload(const std::string& path){
  std::ifstream f(path);
  if(!f) return;
  std::string line, section;
  while(std::getline(f, line)){
    auto h = trim(line);
    if(h.empty() || h[0] == '#' || h[0] == ';') continue;
    if(h.front() == '[' && h.back() == ']'){
      section = trim(h.substr(1, h.size() - 2));
      continue;
    }
    auto eq = h.find('=');
    if(eq == std::string::npos) continue;
    std::string k = trim(h.substr(0, eq));
    std::string v = trim(h.substr(eq + 1));
    if(section == "engine"){
      if(k == "interval_ms") interval_ms = (uint32_t)parse_u64(v, interval_ms);
      else if(k == "psi_threshold") psi_threshold = parse_dbl(v, psi_threshold);
      else if(k == "psi_low") psi_low = parse_dbl(v, psi_low);
      else if(k == "min_score") min_score = parse_dbl(v, min_score);
      else if(k == "min_reclaimable_mb") min_reclaimable_mb = parse_u64(v, min_reclaimable_mb);
      else if(k == "max_targets") max_targets = (uint32_t)parse_u64(v, max_targets);
      else if(k == "max_reclaim_mb_per_cycle") max_reclaim_mb_per_cycle = parse_u64(v, max_reclaim_mb_per_cycle);
      else if(k == "anon_ratio_cap") anon_ratio_cap = parse_dbl(v, anon_ratio_cap);
      else if(k == "enable_heap_trim") enable_heap_trim = parse_bool(v, enable_heap_trim);
      else if(k == "enable_cgroup_reclaim") enable_cgroup_reclaim = parse_bool(v, enable_cgroup_reclaim);
      else if(k == "enable_ksm") enable_ksm = parse_bool(v, enable_ksm);
      else if(k == "dry_run") dry_run = parse_bool(v, dry_run);
    } else if(section == "ksm"){
      // géré par le démon directement
    } else if(section == "preload"){
      if(k == "trim_after_mb") preload.trim_after_mb = parse_u64(v, preload.trim_after_mb);
      else if(k == "free_bytes") preload.free_bytes = parse_u64(v, preload.free_bytes);
    } else if(section == "log"){
      if(k == "level"){
        std::string t = v; for(auto& ch : t) ch = (char)tolower(ch);
        if(t == "debug") log_level = LogLevel::Debug;
        else if(t == "info") log_level = LogLevel::Info;
        else if(t == "warn" || t == "warning") log_level = LogLevel::Warn;
        else if(t == "error") log_level = LogLevel::Error;
      }
    }
  }
}

GlobalStats g_stats{};

} // namespace memflux