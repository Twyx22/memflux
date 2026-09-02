#pragma once
// Chargeur eBPF : attache le traceur de page faults (tracepoint exceptions)
// et expose les compteurs par processus. Nécessite libbpf + l'objet eBPF
// compilé au build. Optionnel : sans objet compilé, le backend est désactivé.
#include "memflux/common.hpp"
#include <map>

namespace memflux::ebpf {

bool available();               // objet compilé + tracepoint présents
bool load();                    // charge + attache au tracepoint
void unload();
bool loaded();

// lit et remet à zéro les compteurs (deltas depuis le dernier appel)
std::map<pid_t, uint64_t> drain_faults();

} // namespace memflux::ebpf