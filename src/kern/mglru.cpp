#include "memflux/kern.hpp"
#include <fstream>

namespace memflux::kern::mglru {

// MGLRU (multi-gen LRU) : /sys/kernel/mm/lru_gen/enabled
// bitmap de features : bit0=working set management, bit1=working set
// protection, bit2=bloom filters. min_ttl_ms : âge minimal avant reclaim.

bool available(){
  std::ifstream f("/sys/kernel/mm/lru_gen/enabled");
  return f.good();
}

bool enabled(){
  std::ifstream f("/sys/kernel/mm/lru_gen/enabled");
  uint64_t v = 0;
  return (bool)(f >> v) && (v & 0x1);
}

bool enable(bool on){
  std::ofstream f("/sys/kernel/mm/lru_gen/enabled");
  if(!f) return false;
  // 0x0007 = toutes features (bits 0-2)
  f << (on ? "0x0007" : "0x0000");
  return !f.fail();
}

bool set_min_ttl_ms(uint64_t ms){
  std::ofstream f("/sys/kernel/mm/lru_gen/min_ttl_ms");
  if(!f) return false;
  f << ms;
  return !f.fail();
}

uint64_t get_min_ttl_ms(){
  std::ifstream f("/sys/kernel/mm/lru_gen/min_ttl_ms");
  uint64_t v = 0;
  return (f >> v) ? v : 0;
}

} // namespace memflux::kern::mglru