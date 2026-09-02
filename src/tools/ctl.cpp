#include "memflux/ipc.hpp"
#include <cstdio>
#include <cstring>

using namespace memflux;

int main(int argc, char** argv){
  if(argc < 2){
    fprintf(stderr,
      "usage: memfluxctl <status|top [n]|pause|resume|kick|force|adjust <psi_x1000>>\n");
    return 1;
  }
  int err = 0;
  std::string r;
  if(!strcmp(argv[1], "status")) r = ipc::send_request(ipc::C_STATUS, 0, &err);
  else if(!strcmp(argv[1], "top")){
    uint32_t n = argc > 2 ? (uint32_t)atoi(argv[2]) : 10;
    r = ipc::send_request(ipc::C_TOP, n, &err);
  }
  else if(!strcmp(argv[1], "pause")) r = ipc::send_request(ipc::C_PAUSE, 0, &err);
  else if(!strcmp(argv[1], "resume")) r = ipc::send_request(ipc::C_RESUME, 0, &err);
  else if(!strcmp(argv[1], "kick")) r = ipc::send_request(ipc::C_KICK, 0, &err);
  else if(!strcmp(argv[1], "force")) r = ipc::send_request(ipc::C_FORCE, 0, &err);
  else if(!strcmp(argv[1], "adjust")){
    if(argc < 3){ fprintf(stderr, "adjust <psi x1000, ex: 150>\n"); return 1; }
    r = ipc::send_request(ipc::C_ADJUST, (uint32_t)atoi(argv[2]), &err);
  }
  else { fprintf(stderr, "cmd inconnue: %s\n", argv[1]); return 1; }

  if(r.empty()){
    fprintf(stderr, "memfluxd injoignable (errno=%d) — est-il lancé ?\n", err);
    return 2;
  }
  printf("%s\n", r.c_str());
  return 0;
}