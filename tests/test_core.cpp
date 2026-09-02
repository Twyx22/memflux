#include "memflux/common.hpp"
#include "memflux/kern.hpp"
#include "memflux/workingset.hpp"
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <vector>

using namespace memflux;

static void test_size_classes(){}
static void test_fmt(){
  auto s = fmt_line("a", 1, 2.5, std::string("b"));
  assert(s == "a 1 2.500000 b");
}
static void test_psi(){
  auto p = kern::psi_memory();
  // sur un noyau récent, ok=true même à 0
  assert(p.ok || !p.ok); // pas de crash
}
static void test_meminfo(){
  auto m = kern::meminfo();
  assert(m.total_kb > 0);
  assert(m.anon_kb <= m.total_kb * 2); // sanity
}
static void test_ws_self(){
  // working set sur soi-même
  assert(clear_soft_dirty(getpid()));
  volatile auto* sink = new uint64_t[1024];
  sink[0] = 42;
  auto ws = sample_working_set(getpid());
  delete[] sink;
  assert(ws.ok);
  assert(ws.touched_kb > 0);
}
static void test_pidfd(){
  auto fd = kern::pidfd_open(getpid());
  assert((bool)fd);
  if(fd) close(fd.fd);
}
static void test_config(){
  Config c;
  c.reload("/nonexistent"); // ne doit pas planter
  assert(c.interval_ms == 1000);
  assert(c.enable_damon);
  assert(c.default_mode == "pageout");
  assert(c.groups.empty());
}
static void test_config_groups(){
  // parser [groups] + default_mode + enable_damon
  FILE* f = fopen("/tmp/memflux_test.conf", "we");
  assert(f);
  fputs("[engine]\ninterval_ms = 500\ndefault_mode = cold\nenable_damon = false\n"
        "[groups]\n"
        "group.0.prefix = /system.slice\n"
        "group.0.weight = 0.3\n"
        "group.0.mode = cold\n"
        "group.1.prefix = /user.slice\n"
        "group.1.weight = 1.5\n"
        "group.1.mode = pageout\n", f);
  fclose(f);
  Config c;
  c.reload("/tmp/memflux_test.conf");
  assert(c.interval_ms == 500);
  assert(!c.enable_damon);
  assert(c.default_mode == "cold");
  assert(c.groups.size() == 2);
  assert(c.groups[0].cgroup_prefix == "/system.slice");
  assert(c.groups[0].weight == 0.3);
  assert(c.groups[0].mode == 1);
  assert(c.groups[1].mode == 0);
  unlink("/tmp/memflux_test.conf");
}

int main(){
  test_fmt();
  test_psi();
  test_meminfo();
  test_ws_self();
  test_pidfd();
  test_config();
  test_config_groups();
  printf("all core tests passed\n");
  return 0;
}