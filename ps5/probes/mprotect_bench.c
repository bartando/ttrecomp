// SPDX-License-Identifier: GPL-3.0-or-later
// elfldr payload: what a single 16 KiB mprotect costs on the console, and
// whether the mapping type (anonymous vs direct memory, aliased or not) or
// other busy cores (TLB shootdowns) change it. The title measured ~33 us per
// call; macOS is ~1 us.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                      size_t alignment, int32_t memory_type, int64_t* start);
int32_t sceKernelReleaseDirectMemory(int64_t start, size_t length);
int32_t sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                                 int64_t start, size_t alignment);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelMunmap(void* address, size_t length);

#define PAGE 0x4000
#define ARENA (64u << 20)
#define ROUNDS 2000

static double now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

// Alternates read-only/read-write on scattered single pages, like the
// shared-memory watch does.
static void bench(const char* name, uint8_t* base) {
  if (!base) {
    printf("bench %-28s skipped\n", name);
    return;
  }
  memset(base, 1, ARENA);
  double protect_total = 0, unprotect_total = 0, worst = 0;
  int failures = 0;
  for (int i = 0; i < ROUNDS; ++i) {
    uint8_t* page = base + (size_t)((i * 37) % (ARENA / PAGE)) * PAGE;
    double t0 = now_us();
    failures += mprotect(page, PAGE, PROT_READ) != 0;
    double t1 = now_us();
    failures += mprotect(page, PAGE, PROT_READ | PROT_WRITE) != 0;
    double t2 = now_us();
    protect_total += t1 - t0;
    unprotect_total += t2 - t1;
    if (t1 - t0 > worst) worst = t1 - t0;
  }
  // One call covering many pages, to see whether cost is per call or per page.
  double t0 = now_us();
  for (int i = 0; i < 100; ++i) {
    mprotect(base, 64 * PAGE, PROT_READ);
    mprotect(base, 64 * PAGE, PROT_READ | PROT_WRITE);
  }
  double wide = (now_us() - t0) / 200;
  printf("bench %-28s protect=%.1fus unprotect=%.1fus worst=%.1fus 64-page=%.1fus fail=%d\n",
         name, protect_total / ROUNDS, unprotect_total / ROUNDS, worst, wide, failures);
}

static atomic_int spinning_stop;
static void* spin(void* arg) {
  (void)arg;
  while (!atomic_load_explicit(&spinning_stop, memory_order_relaxed)) {
  }
  return 0;
}

static uint8_t* map_direct(int64_t start, void* fixed) {
  void* address = fixed;
  if (sceKernelMapDirectMemory(&address, ARENA, PROT_READ | PROT_WRITE, fixed ? 0x10 : 0, start,
                               0x10000) != 0) {
    return 0;
  }
  return address;
}

int main(void) {
  uint8_t* anon = mmap(0, ARENA, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (anon == MAP_FAILED) anon = 0;
  bench("anonymous", anon);

  int64_t direct = -1;
  int status = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), ARENA, 0x200000,
                                             12, &direct);
  uint8_t* direct_a = status == 0 ? map_direct(direct, 0) : 0;
  if (status) printf("direct allocate failed 0x%08x\n", (unsigned)status);
  bench("direct", direct_a);
  uint8_t* direct_b = direct_a ? map_direct(direct, 0) : 0;
  bench("direct, second alias mapped", direct_a && direct_b ? direct_a : 0);

  pthread_t threads[12];
  int started = 0;
  for (; started < 12; ++started) {
    if (pthread_create(&threads[started], 0, spin, 0)) break;
  }
  printf("spinning threads: %d\n", started);
  bench("anonymous, busy cores", anon);
  bench("direct+alias, busy cores", direct_a && direct_b ? direct_a : 0);
  atomic_store(&spinning_stop, 1);
  for (int i = 0; i < started; ++i) pthread_join(threads[i], 0);

  if (direct_b) sceKernelMunmap(direct_b, ARENA);
  if (direct_a) sceKernelMunmap(direct_a, ARENA);
  if (direct >= 0) sceKernelReleaseDirectMemory(direct, ARENA);
  if (anon) munmap(anon, ARENA);
  printf("mprotect bench: done\n");
  return 0;
}
