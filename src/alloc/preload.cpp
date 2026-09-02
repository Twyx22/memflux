// memflux-preload : allocateur interposé (LD_PRELOAD) qui réduit l'RSS des
// applications sans modification du code source.
//
// Techniques (niveau jemalloc/tcmalloc) :
//   • réservation d'espace d'adressage 1 Tio (PROT_NONE, MAP_NORESERVE)
//     → test d'ownership par bornes, jamais de lecture mémoire étrangère
//   • runs de 2 Mio alignés, carving par classe + bitmap libre
//   • thread-cache par classe (hot path sans verrou)
//   • purge madvise(MADV_DONTNEED) des blocs libres >= 4 Kio, runs
//     entièrement libres inactifs → munmap (rendu au noyau)
//   • grandes allocations : mmap dédié dans la réserve, munmap au free
//   • fork-safe, init réentrance-safe, délégation libc pour les pointeurs
//     alloués avant interposition
//
// Env : MEMFLUX_TRIM_MS, MEMFLUX_RUN_IDLE_MS, MEMFLUX_DIRTY_IDLE_MS,
//       MEMFLUX_STATS=1
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern "C" {

// ------------------------------------------------------------------ réglages
static size_t   env_trim_interval_ms = 3000;
static uint64_t env_run_idle_ms       = 6000;
static uint64_t env_dirty_idle_ms     = 2000;
static int      env_stats             = 0;

static uint64_t now_ms(){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void read_env(){
  if(const char* e = getenv("MEMFLUX_TRIM_MS"))       env_trim_interval_ms = (size_t)atoll(e);
  if(const char* e = getenv("MEMFLUX_RUN_IDLE_MS"))   env_run_idle_ms      = (uint64_t)atoll(e);
  if(const char* e = getenv("MEMFLUX_DIRTY_IDLE_MS")) env_dirty_idle_ms    = (uint64_t)atoll(e);
  if(const char* e = getenv("MEMFLUX_STATS"))         env_stats            = atoi(e);
}

// ------------------------------------------------------------------ classes
static const size_t kSizes[] = {
  8,16,24,32,40,48,56,64,80,96,112,128,
  160,192,224,256,320,384,448,512,640,768,896,1024,
  1280,1536,1792,2048,2560,3072,3584,4096,5120,6144,7168,8192,
  10240,12288,14336,16384,20480,24576,28672,32768,40960,49152,57344,65536,
  81920,98304,114688,131072,163840,196608,229376,262144,327680,393216,458752,524288,
  655360,786432,917504,1048576,1310720,1572864,1835008,2097152,
};
static constexpr unsigned kNumClasses = sizeof(kSizes) / sizeof(kSizes[0]);
static constexpr size_t   kLargeMin   = 2097153;
static constexpr size_t   kRunSize    = 2u << 20;
static constexpr uint32_t kMaxElems   = 65536;

static unsigned class_of(size_t n){
  unsigned lo = 0, hi = kNumClasses;
  while(lo < hi){
    unsigned mid = (lo + hi) / 2;
    if(kSizes[mid] < n) lo = mid + 1; else hi = mid;
  }
  return lo + 1;
}

// ------------------------------------------------------------------ réserve
// 1 Tio d'espace virtuel PROT_NONE : toute adresse qui n'y est pas ne nous
// appartient pas (test O(1) sans déréférencement).
static constexpr size_t kReserveSize = 1ull << 40; // 1 TiB
static uintptr_t g_res_start = 0, g_res_end = 0;
static std::atomic<uintptr_t> g_res_bump{0};

static bool reserve_init(){
  void* p = mmap(nullptr, kReserveSize, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if(p == MAP_FAILED) return false;
  g_res_start = (uintptr_t)p;
  g_res_end   = g_res_start + kReserveSize;
  g_res_bump  = g_res_start;
  return true;
}

static inline bool in_reserve(void* p){
  uintptr_t a = (uintptr_t)p;
  return a >= g_res_start && a < g_res_end;
}

// alloue un fragment aligné de la réserve, mappé RW
static void* reserve_map(size_t size, size_t align){
  uintptr_t want = g_res_bump.fetch_add(size + align);
  uintptr_t base = (want + align - 1) & ~(uintptr_t)(align - 1);
  if(base + size > g_res_end) return nullptr; // réserve épuisée
  void* p = mmap((void*)base, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if(p == MAP_FAILED){
    // rollback best-effort : on marque l'espace consommé, on réessaie plus loin
    return nullptr;
  }
  return p;
}

// ------------------------------------------------------------------ run
struct Run {
  uint32_t magic;                    // kMagicRun
  uint32_t cls;                      // 1-based
  uint32_t elems;
  uint32_t used;
  uint32_t elem;
  uint32_t pad;
  Run*     next;
  Run*     prev;
  uint64_t last_fully_free_ms;
  uint64_t last_touch_ms;
  uint64_t bits[];                   // 1 = libre
};

static constexpr uint32_t kMagicRun   = 0x4D465552u; // 'MFUR'
static constexpr uint32_t kMagicLarge = 0x4D46554Cu; // 'MFUL'

struct LargeHdr {
  uint32_t magic;
  uint32_t pad;
  size_t   map_size;
  size_t   user;
};

struct ClassState {
  Run*     head = nullptr;
  uint64_t live_blocks = 0;
};

static ClassState      g_cls[kNumClasses];
static pthread_mutex_t g_mu[kNumClasses];
static pthread_once_t  g_init_once = PTHREAD_ONCE_INIT;
static std::atomic<bool> g_ready{false};
static std::atomic<uint64_t> g_mapped{0}, g_freed_to_os{0}, g_purged{0};
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;

static inline unsigned bits_words(uint32_t e){ return (e + 63) / 64; }
// décalage des données arrondi page : sizeof(Run)+bitmap aligné sur 4 Kio
static inline size_t data_offset_padded(uint32_t e){
  size_t b = sizeof(Run) + bits_words(e) * 8;
  return (b + 4095) & ~(size_t)4095;
}
static inline size_t   run_bytes(uint32_t elems, uint32_t elem){
  return data_offset_padded(elems) + (size_t)elems * elem;
}

static void run_insert(unsigned c, Run* r){
  r->prev = nullptr;
  r->next = g_cls[c].head;
  if(g_cls[c].head) g_cls[c].head->prev = r;
  g_cls[c].head = r;
}
static void run_remove(unsigned c, Run* r){
  if(r->prev) r->prev->next = r->next; else g_cls[c].head = r->next;
  if(r->next) r->next->prev = r->prev;
}

static Run* run_new(unsigned c){
  uint32_t elems = (uint32_t)(kRunSize / kSizes[c - 1]);
  if(elems > kMaxElems) elems = kMaxElems;
  uint32_t elem = (uint32_t)kSizes[c - 1];
  size_t need = run_bytes(elems, elem);
  size_t map = (need + 4095) & ~(size_t)4095;
  Run* r;
  if(g_res_start){
    pthread_mutex_lock(&g_slot_mu);
    r = (Run*)reserve_map(map, kRunSize);
    pthread_mutex_unlock(&g_slot_mu);
  } else {
    r = (Run*)mmap(nullptr, map, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(r == MAP_FAILED) r = nullptr;
  }
  if(!r) return nullptr;
  r->magic = kMagicRun;
  r->cls = c;
  r->elems = elems;
  r->used = 0;
  r->elem = elem = (uint32_t)kSizes[c - 1];
  r->pad = 0;
  r->last_fully_free_ms = now_ms();
  r->last_touch_ms = r->last_fully_free_ms;
  memset(r->bits, 0xFF, bits_words(elems) * 8);
  if(elems & 63) r->bits[elems / 64] &= (1ULL << (elems & 63)) - 1;
  g_mapped += map;
  run_insert(c - 1, r);
  return r;
}

static inline uint8_t* run_data(Run* r){
  return (uint8_t*)r + data_offset_padded(r->elems);
}
static inline Run* run_of(void* p){
  return (Run*)((uintptr_t)p & ~(uintptr_t)(kRunSize - 1));
}
// précondition : in_reserve(p) — aucune lecture mémoire étrangère
static bool ours_small(void* p){
  Run* r = run_of(p);
  if(r->magic != kMagicRun || r->cls == 0 || r->cls > kNumClasses) return false;
  uint8_t* d = run_data(r);
  if((uint8_t*)p < d) return false;
  uintptr_t off = (uint8_t*)p - d;
  return off < (uintptr_t)r->elems * r->elem && (off % r->elem) == 0;
}
static bool ours_large(void* p){
  auto* h = (LargeHdr*)((uint8_t*)p - sizeof(LargeHdr));
  return h->magic == kMagicLarge;
}

static void* run_take(Run* r){
  uint32_t w = bits_words(r->elems);
  for(uint32_t i = 0; i < w; ++i){
    uint64_t b = r->bits[i];
    if(!b) continue;
    int bit = __builtin_ctzll(b);
    uint32_t idx = i * 64 + (uint32_t)bit;
    if(idx >= r->elems) continue;
    r->bits[i] = b & (b - 1);
    r->used++;
    if(r->used == 1) r->last_fully_free_ms = 0;
    r->last_touch_ms = now_ms();
    g_cls[r->cls - 1].live_blocks++;
    return run_data(r) + (size_t)idx * r->elem;
  }
  return nullptr;
}
static void run_give(Run* r, void* p){
  uint8_t* d = run_data(r);
  uint32_t idx = (uint32_t)(((uint8_t*)p - d) / r->elem);
  r->bits[idx / 64] |= 1ULL << (idx % 64);
  if(r->used) r->used--;
  r->last_touch_ms = now_ms();
  if(r->used == 0 && r->last_fully_free_ms == 0) r->last_fully_free_ms = now_ms();
  g_cls[r->cls - 1].live_blocks--;
}

// ------------------------------------------------------------------ tcache
static constexpr uint8_t kTCap = 16;
struct TCache {
  void*   buf[kNumClasses][kTCap];
  uint8_t n[kNumClasses];
};

static pthread_key_t tkey;
static bool          tkey_ok = false;

static void tcache_dtor(void* v){
  auto* tc = (TCache*)v;
  for(unsigned c = 0; c < kNumClasses; ++c){
    for(uint8_t i = 0; i < tc->n[c]; ++i){
      void* p = tc->buf[c][i];
      if(in_reserve(p) && ours_small(p)){
        Run* r = run_of(p);
        pthread_mutex_lock(&g_mu[r->cls - 1]);
        run_give(r, p);
        pthread_mutex_unlock(&g_mu[r->cls - 1]);
      }
    }
  }
  munmap(tc, sizeof(TCache));
}

static TCache* tcache(){
  if(!tkey_ok) return nullptr;
  auto* tc = (TCache*)pthread_getspecific(tkey);
  if(!tc){
    tc = (TCache*)mmap(nullptr, sizeof(TCache), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(tc == MAP_FAILED) return nullptr;
    memset(tc, 0, sizeof *tc);
    pthread_setspecific(tkey, tc);
  }
  return tc;
}

// ------------------------------------------------------------------ init
static void do_init(){
  read_env();
  for(unsigned i = 0; i < kNumClasses; ++i) pthread_mutex_init(&g_mu[i], nullptr);
  tkey_ok = pthread_key_create(&tkey, tcache_dtor) == 0;
  g_ready = true;
}

static void* trim_thread(void*);
static void start_trim(){
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 256 * 1024);
  if(pthread_create(&th, &at, trim_thread, nullptr) == 0)
    pthread_setname_np(th, "memflux-trim");
  pthread_attr_destroy(&at);
}

static void atfork_prepare(){
  if(!g_ready) return;
  pthread_mutex_lock(&g_slot_mu);
  for(unsigned i = 0; i < kNumClasses; ++i) pthread_mutex_lock(&g_mu[i]);
}
static void atfork_release(){
  for(unsigned i = 0; i < kNumClasses; ++i) pthread_mutex_unlock(&g_mu[i]);
  pthread_mutex_unlock(&g_slot_mu);
}

static void ensure_init(){
  static std::atomic<bool> started{false};
  if(started.load(std::memory_order_acquire)) return;
  // réserve d'abord (jamais après des threads actifs)
  static std::atomic<bool> res_done{false};
  bool exp = false;
  if(res_done.compare_exchange_strong(exp, true)){
    if(!g_res_start) reserve_init();
  }
  if(pthread_once(&g_init_once, do_init) == 0 && g_ready){
    bool exp2 = false;
    if(started.compare_exchange_strong(exp2, true)){
      pthread_atfork(atfork_prepare, atfork_release, atfork_release);
      start_trim();
      atexit([]{ if(env_stats) fprintf(stderr,
        "[memflux-preload] mapped=%lluMB purged=%lluMB released=%lluMB\n",
        (unsigned long long)(g_mapped >> 20),
        (unsigned long long)(g_purged >> 20),
        (unsigned long long)(g_freed_to_os >> 20)); });
    }
  }
}

// ------------------------------------------------------------------ purge
static void purge_run(Run* r, uint64_t t){
  if(r->used == 0 && t - r->last_fully_free_ms >= env_run_idle_ms){
    run_remove(r->cls - 1, r);
    uint32_t elems = r->elems, elem = r->elem;
    size_t map = run_bytes(elems, elem);
    map = (map + 4095) & ~(size_t)4095;
    munmap(r, map);
    g_freed_to_os += map;
    return;
  }
  if(t - r->last_touch_ms < env_dirty_idle_ms) return;
  r->last_touch_ms = t;
  // Purge par page complète : une page de 4 Kio n'est rendue que si TOUS
  // les blocs qui la chevauchent sont libres (jamais de bloc vivant écrasé).
  // Cas couverts : elem divise 4096 ou en est multiple aligné. Sinon skip.
  size_t elem = r->elem;
  if(elem >= 4096 && (elem & 4095) != 0) return;
  uint8_t* d = run_data(r);
  size_t data_len = (size_t)r->elems * elem;
  for(size_t off = 0; off + 4096 <= data_len; off += 4096){
    uint32_t b0 = (uint32_t)(off / elem);
    uint32_t b1 = (uint32_t)((off + 4096) / elem);
    if(b1 > r->elems) b1 = r->elems;
    if(b1 <= b0) break;
    bool all_free = true;
    for(uint32_t b = b0; b < b1; ++b){
      if(!(r->bits[b >> 6] & (1ULL << (b & 63)))){ all_free = false; break; }
    }
    if(all_free){
      madvise(d + off, 4096, MADV_DONTNEED);
      g_purged += 4096;
    }
  }
}

static void* trim_thread(void*){
  for(;;){
    struct timespec ts{ (time_t)(env_trim_interval_ms / 1000),
                        (long)((env_trim_interval_ms % 1000) * 1000000L) };
    nanosleep(&ts, nullptr);
    if(!g_ready) continue;
    uint64_t t = now_ms();
    for(unsigned c = 0; c < kNumClasses; ++c){
      pthread_mutex_lock(&g_mu[c]);
      Run* r = g_cls[c].head;
      while(r){
        Run* nx = r->next;
        purge_run(r, t);
        r = nx;
      }
      pthread_mutex_unlock(&g_mu[c]);
    }
  }
  return nullptr;
}

// ------------------------------------------------------------------ API
void* malloc(size_t n){
  if(n == 0) n = 1;
  if(!g_ready.load(std::memory_order_acquire)){
    ensure_init();
    if(!g_ready) return nullptr; // réserve impossible → pas d'alloc
  }
  if(n >= kLargeMin){
    size_t need = sizeof(LargeHdr) + n;
    size_t map = (need + 4095) & ~(size_t)4095;
    uint8_t* raw;
    if(g_res_start){
      pthread_mutex_lock(&g_slot_mu);
      raw = (uint8_t*)reserve_map(map, 4096);
      pthread_mutex_unlock(&g_slot_mu);
    } else {
      raw = (uint8_t*)mmap(nullptr, map, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if(raw == MAP_FAILED) raw = nullptr;
    }
    if(!raw) return nullptr;
    auto* h = (LargeHdr*)raw;
    h->magic = kMagicLarge;
    h->map_size = map;
    h->user = n;
    g_mapped += map;
    return raw + sizeof(LargeHdr);
  }
  unsigned c = class_of(n) - 1;
  if(tkey_ok){
    auto* tc = (TCache*)pthread_getspecific(tkey);
    if(tc && tc->n[c]) return tc->buf[c][--tc->n[c]];
  }
  void* p = nullptr;
  pthread_mutex_lock(&g_mu[c]);
  Run* r = g_cls[c].head;
  while(r && r->used == r->elems) r = r->next;
  if(!r) r = run_new(c + 1);
  if(r) p = run_take(r);
  pthread_mutex_unlock(&g_mu[c]);
  return p;
}

void free(void* p){
  if(!p) return;
  // pointeur hors réserve → jamais à nous (test bornes, pas de déréf)
  if(g_res_start && !in_reserve(p)){
    static void (*rf)(void*) = nullptr;
    if(!rf) rf = (void(*)(void*))dlsym(RTLD_NEXT, "free");
    if(rf) rf(p);
    return;
  }
  if(in_reserve(p) && ours_small(p)){
    unsigned c = run_of(p)->cls - 1;
    if(tkey_ok){
      auto* tc = (TCache*)pthread_getspecific(tkey);
      if(tc && tc->n[c] < kTCap){
        tc->buf[c][tc->n[c]++] = p;
        return;
      }
    }
    Run* r = run_of(p);
    pthread_mutex_lock(&g_mu[c]);
    run_give(r, p);
    pthread_mutex_unlock(&g_mu[c]);
    return;
  }
  if(in_reserve(p) && ours_large(p)){
    auto* h = (LargeHdr*)((uint8_t*)p - sizeof(LargeHdr));
    munmap(h, h->map_size);
    g_freed_to_os += h->map_size;
    return;
  }
  // dans la réserve mais ni run ni large : corruption — ignore
}

void* calloc(size_t nm, size_t sz){
  size_t tot;
  if(__builtin_mul_overflow(nm, sz, &tot)){ errno = ENOMEM; return nullptr; }
  void* p = malloc(tot);
  // pages mmap fraîches = zéro, mais runs recyclés peuvent contenir des
  // données : on zero-initialise toujours (sûr et simple)
  if(p) memset(p, 0, tot);
  return p;
}

void* realloc(void* p, size_t n){
  if(!p) return malloc(n);
  if(n == 0){ free(p); return malloc(1); }
  size_t old = 0;
  bool ours = false;
  if(g_res_start && in_reserve(p)){
    if(ours_small(p)){ old = run_of(p)->elem; ours = true; }
    else if(ours_large(p)){ old = ((LargeHdr*)((uint8_t*)p - sizeof(LargeHdr)))->user; ours = true; }
  }
  if(!ours){
    static void* (*rr)(void*, size_t) = nullptr;
    if(!rr) rr = (void*(*)(void*,size_t))dlsym(RTLD_NEXT, "realloc");
    return rr ? rr(p, n) : nullptr;
  }
  if(n <= old && old < kLargeMin && n > (old / 2)) return p;
  void* np = malloc(n);
  if(np){
    memcpy(np, p, old < n ? old : n);
    free(p);
  }
  return np;
}

int posix_memalign(void** out, size_t align, size_t n){
  if(!out) return EINVAL;
  if(align < sizeof(void*)) align = sizeof(void*);
  if((align & (align - 1)) != 0) return EINVAL;
  size_t need = n + align + 4096;
  void* p = malloc(need);
  if(!p) return ENOMEM;
  uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
  *out = (void*)a;
  return 0;
}

void* aligned_alloc(size_t align, size_t n){
  void* out = nullptr;
  if(posix_memalign(&out, align ? align : 1, n) != 0) return nullptr;
  return out;
}
void* memalign(size_t align, size_t n){ return aligned_alloc(align, n); }
void* valloc(size_t n){ return aligned_alloc((size_t)sysconf(_SC_PAGESIZE), n); }
void* pvalloc(size_t n){
  size_t pg = (size_t)sysconf(_SC_PAGESIZE);
  return aligned_alloc(pg, (n + pg - 1) & ~(pg - 1));
}
void* reallocarray(void* p, size_t nm, size_t sz){
  size_t tot;
  if(__builtin_mul_overflow(nm, sz, &tot)){ errno = ENOMEM; return nullptr; }
  return realloc(p, tot);
}

} // extern "C"