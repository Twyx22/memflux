#include "memflux/ebpf.hpp"
#include <bpf/libbpf.h>
#include <sys/stat.h>
#include <linux/bpf.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace memflux::ebpf {

// Implémentation libbpf (l'objet eBPF est compilé par clang au build).
struct BpfState {
  struct bpf_object* obj = nullptr;
  struct bpf_program* prog = nullptr;
  struct bpf_map* map = nullptr;
  struct bpf_link* link = nullptr;
};

static BpfState g;

bool available(){
  struct stat st;
  return stat(EbpfObjectPath, &st) == 0 &&
         stat("/sys/kernel/tracing/events/exceptions/page_fault_user", &st) == 0;
}

bool load(){
  if(g.obj) return true;
  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
  g.obj = bpf_object__open(EbpfObjectPath);
  if(!g.obj){
    LOG_WARN("ebpf: cannot open object ", EbpfObjectPath);
    return false;
  }
  if(bpf_object__load(g.obj) != 0){
    LOG_WARN("ebpf: object load failed (BTF/kernel mismatch?)");
    bpf_object__close(g.obj);
    g.obj = nullptr;
    return false;
  }
  g.prog = bpf_object__find_program_by_name(g.obj, "on_page_fault_user");
  g.map = bpf_object__find_map_by_name(g.obj, "minor_faults");
  if(!g.prog || !g.map){
    LOG_WARN("ebpf: program/map not found in object");
    bpf_object__close(g.obj);
    g.obj = nullptr;
    return false;
  }
  g.link = bpf_program__attach_tracepoint(
      g.prog, "exceptions", "page_fault_user");
  if(!g.link){
    LOG_WARN("ebpf: attach tracepoint failed");
    bpf_object__close(g.obj);
    g.obj = nullptr;
    return false;
  }
  LOG_INFO("eBPF page-fault tracer attached (exceptions/page_fault_user)");
  return true;
}

void unload(){
  if(g.link){ bpf_link__destroy(g.link); g.link = nullptr; }
  if(g.obj){ bpf_object__close(g.obj); g.obj = nullptr; }
}

bool loaded(){ return g.obj != nullptr; }

struct fault_key { __u32 pid; };
struct fault_val { __u64 minor; };

std::map<pid_t, uint64_t> drain_faults(){
  std::map<pid_t, uint64_t> out;
  if(!g.map) return out;
  struct fault_key key = {}, next = {};
  struct fault_val val = {};
  while(bpf_map__get_next_key(g.map, &key, &next, sizeof(next)) == 0){
    if(bpf_map__lookup_elem(g.map, &next, sizeof(next), &val, sizeof(val), 0) == 0
       && val.minor){
      out[(pid_t)next.pid] = val.minor;
      struct fault_val zero = {};
      bpf_map__update_elem(g.map, &next, sizeof(next), &zero, sizeof(zero), BPF_EXIST);
    }
    key = next;
  }
  return out;
}

} // namespace memflux::ebpf