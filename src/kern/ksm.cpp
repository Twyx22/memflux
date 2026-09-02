#include "memflux/kern.hpp"
namespace memflux::kern {
// stub KSM module-level (le pilotage réel passe par sysctl)
bool ksm_stub(){ return true; }
}