<div align="center">

# memflux

**Adaptive memory reclamation for Linux — reclaim dormant RAM without killing processes.**

[![Build](https://img.shields.io/badge/build-CMake%20%2B%20GCC%2016-blue)](#build)
[![Platform](https://img.shields.io/badge/platform-Linux%20≥%205.19-black?logo=linux)](#requirements)
[![Language](https://img.shields.io/badge/language-C%2B%2B20-00599C?logo=c%2B%2B&logoColor=white)](#)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-success)](#)

*A daemon + LD_PRELOAD allocator that measurably shrinks RSS using the most
advanced kernel interfaces available: `process_madvise(MADV_PAGEOUT)`,
pidfd, PSI, soft-dirty pagemap sampling (DAMON-style), cgroup v2
`memory.reclaim` and KSM.*

</div>

---

## Why

Linux is good at reclaiming memory, but only *reactively*: it swaps out
whatever it stumbles upon when pressure already exists, page cache gets
dropped before cold anonymous memory, and desktop apps (Electron, browsers,
messengers) keep hundreds of megabytes of **dormant anonymous memory** in RAM
— allocated once, touched once, never again.

`memflux` attacks this with three layers:

| Layer | What it does |
|---|---|
| **memfluxd** | Daemon that samples each process's working set, scores dormant memory, and pages it out cross-process — no kill, no freeze, no ptrace attach |
| **libmemflux-preload** | Drop-in jemalloc-style allocator with automatic page purging for any app you launch |
| **memfluxctl** | Live control & introspection: top dormant processes, forced cycles, live tuning |

**Measured on a live desktop (32 GB RAM, zram swap):**

```text
$ memfluxctl force                     # 3 GB dormant anonymous process
forced cycle: 2047 MB reclaimed

$ grep VmRSS /proc/<pid>/status
VmRSS:  2 099 324 kB    →   2 172 kB        (-99.9 %)

Under 20 GB synthetic load: 10.4 GB reclaimed across 57 daemon cycles.
```

The process keeps running normally — pages are re-faulted from zram at
microsecond latency on next access.

---

## How it works

```text
                ┌──────────────────────────────────────────────────┐
                │                     memfluxd                     │
                │  every 1 s:                                      │
                │                                                  │
   /proc/*/maps │  1. WORKING-SET SAMPLING        (DAMON-like)     │
   ────────────►│     clear_refs(4) + pagemap soft-dirty scan      │
   /proc/pagemap│     → % of anonymous pages touched per window    │
                │                                                  │
   /proc/pressure  2. SCORING:  anon_size × coldness × swap_press │
   ────────────► │     (PSI memory gate + MemAvailable < 10-15 %)   │
                │                                                  │
   pidfd_open   │  3. RECLAIM dormantes régions:                   │
   ────────────► │     process_madvise(MADV_PAGEOUT)                │
                │     → zram/swap, fault-in on demand              │
   cgroup v2    │  4. PERSISTENT pressure:                         │
   ────────────► │     memory.reclaim (kernel-driven proactive)     │
                │                                                  │
   /sys/.../ksm │  5. KSM auto-enable            (page dedup)      │
                └──────────────────────────────────────────────────┘
```

### Kernel interfaces used

| Interface | Since | Role |
|---|---|---|
| `process_madvise(MADV_PAGEOUT)` | 5.10 / 5.14 | cross-process pageout of dormant regions |
| `pidfd_open()` | 5.3 | race-free process handles (no PID reuse) |
| PSI — `/proc/pressure/memory` | 4.20 | real stall-based memory pressure |
| soft-dirty pagemap + `clear_refs` | 3.11 | per-window working-set sampling, O(VMA), no ptrace |
| cgroup v2 `memory.reclaim` | 5.19 | proactive kernel-driven reclaim |
| KSM sysctls | 2.6.32 | transparent page deduplication |
| `madvise(MADV_DONTNEED)` | always | page-granular allocator purge |

### Safety model

- **Anti-thrash cooldown** — 5 cycles before re-targeting the same process
- **Major-fault shielding** — processes faulting pages in are skipped
- **Working-set window** — an app active in the last second is never targeted
- **Coldness gating** — score is zero if > 25 % of pages were touched
- **zram-first** — on zram systems, pageout = compression in RAM, not disk I/O
- **Force is explicit** — under normal memory availability, the daemon
  only *observes*; reclamation triggers on PSI or low availability

---

## Components

### memfluxd

The daemon. Every cycle it scans `/proc`, samples working sets of large
anonymous processes, computes a reclaim score and — under memory pressure —
pages out the best candidates' dormant regions. Persists nothing, talks
nothing, stays out of the way.

### libmemflux-preload

```bash
LD_PRELOAD=libmemflux-preload.so ./your-app
```

A complete jemalloc-style allocator, ~500 lines:

- **1 TiB PROT_NONE address-space reservation** — ownership checks are
  pointer-range tests (O(1), zero foreign-memory dereference)
- 69 size classes, 2 MiB aligned runs, free bitmaps
- per-thread cache — lock-free hot path
- **whole-page purge**: a 4 KiB page is `madvise(MADV_DONTNEED)`'d only when
  *every* block overlapping it is free — never corrupts live data
- idle runs → `munmap` (returned to kernel)
- fork-safe (`pthread_atfork`), recursion-safe init, libc delegation for
  pre-interposition pointers

Tuning via environment:

```bash
MEMFLUX_TRIM_MS=3000       # purge period
MEMFLUX_DIRTY_IDLE_MS=2000 # min idle before purging partial runs
MEMFLUX_RUN_IDLE_MS=6000   # min idle before munmapping free runs
MEMFLUX_STATS=1            # stats at exit
```

### memfluxctl

```text
status              counters + memory state
top [n]             top-N reclaim candidates (anon size, WS%, score, swap)
force               immediate reclaim cycle (bypasses PSI gate)
pause / resume      suspend / resume the daemon
adjust <x1000>      live PSI threshold (150 = 15 %)
```

---

## Build

```bash
git clone https://github.com/Twyx22/memflux.git
cd memflux
./scripts/build.sh              # builds + runs tests
./scripts/build.sh --install    # + installs to /usr/local, systemd unit
```

Requirements: Linux ≥ 5.19 (≥ 5.14 without cgroup reclaim), CMake ≥ 3.20,
GCC/Clang with C++20. **Zero external dependencies** — only libc, libpthread
and the Linux kernel.

### Quick demo

```bash
sudo memfluxd -f -v &                    # daemon (root: CAP_SYS_ADMIN)
./memhog 3000 30 &                       # 3 GB process, touched then idle
sleep 5
memfluxctl force                         # → forced cycle: ~1700 MB reclaimed
grep -E 'VmRSS|VmSwap' /proc/$(pgrep memhog)/status
#  VmRSS:     ~5 MB
#  VmSwap:    ~3 GB   (zram)
```

---

## Repository layout

```text
src/kern/        kernel primitives — pidfd, process_madvise, PSI, pagemap,
                 cgroup v2, KSM sysctls
src/core/        working-set sampler, decision engine, config, logging
src/daemon/      daemon main loop + unix-socket IPC
src/alloc/       interposed allocator (LD_PRELOAD)
src/tools/       memhog, bench_alloc, memfluxctl
include/memflux/ public headers
tests/           kernel-layer unit tests (ctest)
config/          default conf + systemd unit
scripts/         build / install helper
```

---

## Requirements

- Linux ≥ 5.19 for full functionality (cgroup `memory.reclaim`);
  core pageout works from 5.14
- root or `CAP_SYS_ADMIN` for `memfluxd` (same privilege class as
  `systemd-oomd` / `earlyoom`)
- zram recommended (swap-out becomes RAM compression)

## Roadmap

- [ ] DAMON (`/sys/kernel/mm/damon`) backend as an alternative sampler
- [ ] eBPF-based access-pattern tracing for short-lived hot regions
- [ ] MGLRU awareness (`/sys/kernel/mm/lru_gen`)
- [ ] per-cgroup policies & weighting
- [ ] `MADV_COLD` only-mode for latency-critical workloads

## License

[MIT](LICENSE) — © 2026 Twyx22