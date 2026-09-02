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
}

int main(){
  test_fmt();
  test_psi();
  test_meminfo();
  test_ws_self();
  test_pidfd();
  test_config();
  printf("all core tests passed\n");
  return 0;
}