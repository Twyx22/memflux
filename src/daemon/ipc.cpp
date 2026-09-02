#include "memflux/ipc.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace memflux::ipc {

std::string send_request(uint32_t cmd, uint32_t arg, int* err){
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0){ if(err) *err = errno; return {}; }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCK, sizeof addr.sun_path - 1);
  if(connect(fd, (sockaddr*)&addr, sizeof addr) < 0){
    if(err) *err = errno;
    close(fd);
    return {};
  }
  uint32_t req[2] = { cmd, arg };
  if(write(fd, req, sizeof req) != sizeof req){
    if(err) *err = errno;
    close(fd);
    return {};
  }
  std::string out;
  char buf[4096];
  ssize_t n;
  while((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
  if(err) *err = 0;
  close(fd);
  return out;
}

} // namespace memflux::ipc