#include "memflux/ebpf.hpp"
namespace memflux::ebpf {
// stub : eBPF indisponible (libbpf ou clang -target bpf absent au build)
bool available(){ return false; }
bool load(){ return false; }
void unload(){}
bool loaded(){ return false; }
std::map<pid_t, uint64_t> drain_faults(){ return {}; }
} // namespace memflux::ebpf