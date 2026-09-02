// ebpf_access.bpf.c — traceur d'accès mémoire léger (tracepoint exceptions).
// Compte les minor/major faults par processus depuis un tracepoint x86 :
//   • page_fault_user : minor faults (première touche / re-touche après swap)
//   • error_code bit 1 = write ; bit 4 = fault sur fetch instruction
// Le démon lit les agrégats chaque cycle pour :
//   • protéger les thrashers (re-fault rate élevé après pageout)
//   • mesurer l'efficacité réelle du pageout
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct fault_key { __u32 pid; };

struct fault_val {
  __u64 minor;       // minor faults (page Fault sur address)
  __u32 pad;
};

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 8192);
  __type(key, struct fault_key);
  __type(value, struct fault_val);
} minor_faults SEC(".maps");

struct trace_event_raw_page_fault_user {
  unsigned short common_type;
  unsigned char common_flags;
  unsigned char common_preempt_count;
  int common_pid;
  unsigned long address;
  unsigned long ip;
  unsigned long error_code;
};

SEC("tracepoint/exceptions/page_fault_user")
int on_page_fault_user(struct trace_event_raw_page_fault_user *ctx){
  __u32 pid = (__u32)bpf_get_current_pid_tgid();
  struct fault_key k = { .pid = pid };
  struct fault_val *v = bpf_map_lookup_elem(&minor_faults, &k);
  if(!v){
    struct fault_val zero = {};
    bpf_map_update_elem(&minor_faults, &k, &zero, BPF_NOEXIST);
    v = bpf_map_lookup_elem(&minor_faults, &k);
    if(!v) return 0;
  }
  v->minor++;
  return 0;
}

char LICENSE[] = "GPL";