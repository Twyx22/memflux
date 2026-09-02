cmake_minimum_required(VERSION 3.20)
project(memflux LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O2 -fno-plt -fstack-protector-strong -Wall -Wextra -Wpedantic)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
  #include <sys/syscall.h>
  #include <unistd.h>
  int main(){ return (int)SYS_process_madvise; }" HAVE_PROCESS_MADVISE)
check_cxx_source_compiles("
  #include <sys/syscall.h>
  #include <unistd.h>
  int main(){ return (int)SYS_pidfd_open; }" HAVE_PIDFD)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# libmemflux : couche noyau + moteur
# ---------------------------------------------------------------------------
add_library(memflux_core STATIC
  src/core/log.cpp
  src/kern/ksm.cpp
  src/kern/cgroup.cpp
  src/kern/sysctl.cpp
  src/kern/process.cpp
  src/kern/psi.cpp
  src/kern/pagemap.cpp
  src/kern/damon.cpp
  src/kern/mglru.cpp
  src/kern/ebpf.cpp
  src/core/workingset.cpp
  src/core/engine.cpp
)
target_include_directories(memflux_core PUBLIC include)
target_link_libraries(memflux_core PUBLIC Threads::Threads bpf)

# eBPF : l'objet est compilé si clang est disponible (optionnel au runtime)
find_program(CLANG_BPF clang)
if(CLANG_BPF)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/ebpf_access.bpf.o
    COMMAND ${CLANG_BPF} -O2 -g -target bpf -D__TARGET_ARCH_x86
            -I/usr/include -c ${CMAKE_SOURCE_DIR}/src/kern/ebpf_access.bpf.c
            -o ${CMAKE_BINARY_DIR}/ebpf_access.bpf.o
    DEPENDS src/kern/ebpf_access.bpf.c
    COMMENT "clang -target bpf: ebpf_access")
  add_custom_target(ebpf_obj ALL
    DEPENDS ${CMAKE_BINARY_DIR}/ebpf_access.bpf.o)
  target_compile_definitions(memflux_core PUBLIC
    EbpfObjectPath="${CMAKE_BINARY_DIR}/ebpf_access.bpf.o")
else()
  target_compile_definitions(memflux_core PUBLIC
    EbpfObjectPath="")
endif()

# ---------------------------------------------------------------------------
# memfluxd : démon
# ---------------------------------------------------------------------------
add_executable(memfluxd src/daemon/main.cpp src/daemon/ipc.cpp)
target_link_libraries(memfluxd PRIVATE memflux_core)

# ---------------------------------------------------------------------------
# memfluxctl : CLI
# ---------------------------------------------------------------------------
add_executable(memfluxctl src/tools/ctl.cpp src/daemon/ipc.cpp)
target_link_libraries(memfluxctl PRIVATE memflux_core)

# ---------------------------------------------------------------------------
# memflux-preload : allocateur interposé LD_PRELOAD
# ---------------------------------------------------------------------------
add_library(memflux_preload SHARED src/alloc/preload.cpp)
target_link_libraries(memflux_preload PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(memflux_preload PROPERTIES
  OUTPUT_NAME memflux-preload
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# outils de test / démo
# ---------------------------------------------------------------------------
add_executable(memhog src/tools/memhog.cpp)
target_link_libraries(memhog PRIVATE Threads::Threads)

add_executable(bench_alloc src/tools/bench_alloc.cpp)
target_link_libraries(bench_alloc PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE memflux_core)
add_test(NAME core COMMAND test_core)

install(TARGETS memfluxd memfluxctl memflux_preload memhog bench_alloc
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib)