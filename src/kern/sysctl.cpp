#include "memflux/kern.hpp"
#include <cstring>

namespace memflux::kern::sysctl {

static bool write_sys(const char* path, const std::string& value){
  FILE* f = fopen(path, "we");
  if(!f) return false;
  bool ok = fwrite(value.c_str(), 1, value.size(), f) == value.size();
  fclose(f);
  return ok;
}

static std::string read_sys(const char* path){
  std::string out;
  FILE* f = fopen(path, "re");
  if(!f) return out;
  char buf[128];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  out.assign(buf, n);
  while(!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

bool ksm_supported(){
  return !read_sys("/sys/kernel/mm/ksm/run").empty();
}

bool ksm_enable(bool on){
  return write_sys("/sys/kernel/mm/ksm/run", on ? "1" : "2");
}

bool ksm_set(const char* name, const std::string& value){
  std::string p = std::string("/sys/kernel/mm/ksm/") + name;
  return write_sys(p.c_str(), value);
}

std::string ksm_get(const char* name){
  std::string p = std::string("/sys/kernel/mm/ksm/") + name;
  return read_sys(p.c_str());
}

} // namespace memflux::kern::sysctl