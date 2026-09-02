#pragma once
#include <string>
#include <vector>
#include <chrono>

namespace memflux {

// ---------------------------------------------------------------- logging
enum class LogLevel { Debug = 0, Info, Warn, Error };

void log_init(LogLevel lvl, const std::string& file = {});
void log_set_level(LogLevel lvl);
LogLevel log_level();
void log_msg(LogLevel lvl, const std::string& msg);
bool log_enabled(LogLevel lvl);

#define LOG_DEBUG(...) do { if(::memflux::log_enabled(::memflux::LogLevel::Debug)) \
  ::memflux::log_msg(::memflux::LogLevel::Debug, ::memflux::fmt_line(__VA_ARGS__)); } while(0)
#define LOG_INFO(...)  do { if(::memflux::log_enabled(::memflux::LogLevel::Info))  \
  ::memflux::log_msg(::memflux::LogLevel::Info,  ::memflux::fmt_line(__VA_ARGS__)); } while(0)
#define LOG_WARN(...)  do { if(::memflux::log_enabled(::memflux::LogLevel::Warn))  \
  ::memflux::log_msg(::memflux::LogLevel::Warn,  ::memflux::fmt_line(__VA_ARGS__)); } while(0)
#define LOG_ERROR(...) do { if(::memflux::log_enabled(::memflux::LogLevel::Error)) \
  ::memflux::log_msg(::memflux::LogLevel::Error, ::memflux::fmt_line(__VA_ARGS__)); } while(0)

// formatters variadiques légers (pas de fmt externe)
template <typename... A>
std::string fmt_line(A&&... a);

namespace detail {
inline void append_one(std::string& s, const std::string& v){ s += v; }
inline void append_one(std::string& s, const char* v){ s += v ? v : "(null)"; }
inline void append_one(std::string& s, char v){ s += v; }
inline void append_one(std::string& s, bool v){ s += v ? "true" : "false"; }
inline void append_one(std::string& s, int v){ s += std::to_string(v); }
inline void append_one(std::string& s, long v){ s += std::to_string(v); }
inline void append_one(std::string& s, long long v){ s += std::to_string(v); }
inline void append_one(std::string& s, unsigned v){ s += std::to_string(v); }
inline void append_one(std::string& s, unsigned long v){ s += std::to_string(v); }
inline void append_one(std::string& s, unsigned long long v){ s += std::to_string(v); }
inline void append_one(std::string& s, double v){ s += std::to_string(v); }
template <typename T> void append_one(std::string& s, const std::vector<T>& v){
  s += '['; bool f = true;
  for(auto& e : v){ if(!f) s += ','; append_one(s, e); f = false; }
  s += ']';
}
inline void join(std::string&){}
template <typename A, typename... R>
void join(std::string& s, A&& a, R&&... r){
  append_one(s, std::forward<A>(a));
  if constexpr (sizeof...(R)) s += ' ';
  join(s, std::forward<R>(r)...);
}
} // namespace detail

template <typename... A>
std::string fmt_line(A&&... a){
  std::string s; s.reserve(96);
  detail::join(s, std::forward<A>(a)...);
  return s;
}

// ---------------------------------------------------------------- config
struct PreloadCfg { uint64_t trim_after_mb = 512; uint64_t free_bytes = 256 << 20; };

struct GroupPolicy {
  std::string cgroup_prefix;
  double weight = 1.0;
  int mode = 0; // 0=pageout, 1=cold
};

struct Config {
  uint32_t interval_ms = 1000;
  double psi_threshold = 0.20;
  double psi_low = 0.05;
  double min_score = 3.0;
  uint64_t min_reclaimable_mb = 64;
  uint32_t max_targets = 3;
  uint64_t max_reclaim_mb_per_cycle = 512;
  double anon_ratio_cap = 0.60;
  bool enable_heap_trim = true;
  bool enable_cgroup_reclaim = true;
  bool enable_ksm = true;
  bool enable_damon = true;          // backend DAMON (fallback pagemap auto)
  std::string default_mode = "pageout"; // "pageout" | "cold"
  std::vector<GroupPolicy> groups;   // politiques par cgroup
  bool dry_run = false;

  PreloadCfg preload;
  LogLevel log_level = LogLevel::Info;

  static Config load(const std::string& path);
  void reload(const std::string& path);
};

// ---------------------------------------------------------------- stats
struct ProcessStat {
  pid_t pid = 0;
  pid_t pid_ns_root = 0;      // pid vu dans le ns racine
  std::string comm;
  std::string cgroup;
  uint64_t rss_kb = 0, anon_kb = 0, file_kb = 0, shmem_kb = 0, swap_kb = 0;
  uint64_t ws_prev_kb = 0;    // working set mesuré au cycle précédent
  double ws_ratio = 1.0;      // ws/rss
  double score = 0.0;
  bool reclaimed_recent = false;
};

struct GlobalStats {
  uint64_t cycles = 0;
  uint64_t pageouts_kb = 0;
  uint64_t heap_trimmed_kb = 0;
  uint64_t cgroup_reclaimed_kb = 0;
  uint64_t errors = 0;
  std::chrono::steady_clock::time_point start{};
};
extern GlobalStats g_stats;

} // namespace memflux