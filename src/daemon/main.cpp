#include "memflux/common.hpp"
#include "memflux/engine.hpp"
#include "memflux/ipc.hpp"
#include <atomic>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace memflux;

namespace {
std::atomic<bool> g_run{true};
std::atomic<bool> g_paused{false};
Config g_cfg;
std::string g_cfg_path = "/etc/memflux.conf";

void on_signal(int){
  g_run = false;
}

int setup_ipc_server(){
  unlink(ipc::SOCK);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ipc::SOCK, sizeof addr.sun_path - 1);
  unlink(ipc::SOCK);
  if(bind(fd, (sockaddr*)&addr, sizeof addr) < 0){ close(fd); return -1; }
  chmod(ipc::SOCK, 0666);
  listen(fd, 4);
  return fd;
}

void handle_client(int cfd, Engine& eng){
  uint32_t req[2] = {0, 0};
  ssize_t n = read(cfd, req, sizeof req);
  if(n != sizeof req){ close(cfd); return; }
  std::string resp;
  switch(req[0]){
    case ipc::C_STATUS: {
      auto m = kern::meminfo();
      char b[512];
      snprintf(b, sizeof b,
        "cycles=%llu pageout_mb=%llu cgroup_mb=%llu errors=%llu\n"
        "mem_total_mb=%llu mem_free_mb=%llu anon_mb=%llu swap_free_mb=%llu\n"
        "psi_thresh=%u paused=%d",
        (unsigned long long)g_stats.cycles,
        (unsigned long long)g_stats.pageouts_kb / 1024,
        (unsigned long long)g_stats.cgroup_reclaimed_kb / 1024,
        (unsigned long long)g_stats.errors,
        (unsigned long long)m.total_kb / 1024,
        (unsigned long long)m.free_kb / 1024,
        (unsigned long long)m.anon_kb / 1024,
        (unsigned long long)m.swap_free_kb / 1024,
        (unsigned)(g_cfg.psi_threshold * 1000), (int)g_paused);
      resp = b;
      break;
    }
    case ipc::C_TOP: {
      auto snap = eng.last_snapshot();
      std::sort(snap.begin(), snap.end(), [](auto& a, auto& b){ return a.score > b.score; });
      size_t max = req[1] ? req[1] : 10;
      char b[256];
      snprintf(b, sizeof b, "%-7s %-20s %10s %8s %8s %8s", "PID", "COMM", "ANON_MB", "WS%", "SCORE", "SWAP_MB");
      resp = b;
      for(size_t i = 0; i < snap.size() && i < max; ++i){
        snprintf(b, sizeof b, "\n%-7d %-20.20s %10llu %8.1f %8.1f %8llu",
                 snap[i].pid, snap[i].comm.c_str(),
                 (unsigned long long)(snap[i].anon_kb / 1024),
                 snap[i].ws_ratio * 100.0, snap[i].score,
                 (unsigned long long)(snap[i].swap_kb / 1024));
        resp += b;
      }
      break;
    }
    case ipc::C_PAUSE: g_paused = true; resp = "paused"; break;
    case ipc::C_RESUME: g_paused = false; resp = "resumed"; break;
    case ipc::C_KICK: g_paused = false; resp = "cycle kicked"; break;
    case ipc::C_FORCE: {
      // cycle forcé (démo/admin) : ignore le gate PSI/avail
      uint64_t kb = eng.force_cycle();
      char b[128];
      snprintf(b, sizeof b, "forced cycle: %llu MB reclaimed",
               (unsigned long long)(kb / 1024));
      resp = b;
      break;
    }
    case ipc::C_ADJUST: {
      double v = (double)req[1] / 1000.0;
      if(v > 0.001 && v < 1.0){ g_cfg.psi_threshold = v; resp = "psi_threshold updated"; }
      else resp = "invalid value";
      break;
    }
    default: resp = "unknown cmd";
  }
  write(cfd, resp.data(), resp.size());
  shutdown(cfd, SHUT_WR);
  close(cfd);
}

int run_daemon(){
  g_cfg = Config::load(g_cfg_path);
  log_init(g_cfg.log_level, "/var/log/memflux.log");

  if(kern::sysctl::ksm_supported()){
    LOG_INFO("KSM support detected");
    if(g_cfg.enable_ksm && kern::sysctl::ksm_enable(true))
      LOG_INFO("KSM enabled (run=1)");
  }

  if(kern::mglru::available()){
    if(!kern::mglru::enabled() && kern::mglru::enable(true))
      LOG_INFO("MGLRU features enabled (0x0007)");
    // protège le working set des applications récemment actives : min_ttl 2 s
    if(kern::mglru::set_min_ttl_ms(2000))
      LOG_INFO("MGLRU min_ttl_ms=2000");
  } else {
    LOG_INFO("MGLRU not available on this kernel");
  }

  Engine eng(g_cfg);
  LOG_INFO("memfluxd started, interval=", g_cfg.interval_ms, "ms");

  int srv = setup_ipc_server();
  if(srv >= 0) fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);

  auto next = std::chrono::steady_clock::now();
  while(g_run){
    // IPC non bloquant
    if(srv >= 0){
      int c = accept(srv, nullptr, nullptr);
      if(c >= 0) handle_client(c, eng);
    }
    if(g_paused){
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    auto now = std::chrono::steady_clock::now();
    if(now >= next){
      eng.run_cycle();
      next = now + std::chrono::milliseconds(g_cfg.interval_ms);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  if(srv >= 0){ close(srv); unlink(ipc::SOCK); }
  LOG_INFO("memfluxd stopped");
  return 0;
}

} // namespace

int main(int argc, char** argv){
  for(int i = 1; i < argc; ++i){
    if(!strcmp(argv[i], "-c") && i + 1 < argc) g_cfg_path = argv[++i];
    else if(!strcmp(argv[i], "-f")) g_cfg_path = "memflux.conf";
    else if(!strcmp(argv[i], "-v")) log_init(LogLevel::Debug);
    else if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")){
      printf("usage: memfluxd [-c conf] [-f] [-v]\n");
      return 0;
    }
  }
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  return run_daemon();
}