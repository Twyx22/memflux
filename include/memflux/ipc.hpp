#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace memflux::ipc {

constexpr const char* SOCK = "/tmp/memflux.sock";

// requêtes monoligne, réponses monoligne (suffisant, sans dépendance)
enum Cmd : uint32_t { C_STATUS = 1, C_PAUSE = 2, C_RESUME = 3, C_KICK = 4, C_ADJUST = 5, C_TOP = 6, C_FORCE = 7 };

struct Request {
  uint32_t cmd = 0;
  uint32_t arg = 0; // générique (ex: nouveau psi x1000)
};

// Réponses texte simples pour status/top.
std::string send_request(uint32_t cmd, uint32_t arg = 0, int* err = nullptr);

} // namespace memflux::ipc