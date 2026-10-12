/*
 * smp_mem_test -- a pool of 20 threads, 5 pinned on each of the 4 cores, that
 * allocate, write, read back and free large blocks of RAM (up to 0.5 GB each)
 * at the same time, 30 operations per thread.
 *
 * The blocks come from the RAM above the first GB, which only this image maps
 * (boards/rpi4b/src/mmu.cpp under UOS_SMP_MEM_TEST): 3008 MB on the 4 GB
 * board, 1024 MB under QEMU raspi4b (2 GB). 20 threads asking for up to
 * 512 MB each want far more than that, so "over memory" is part of the run.
 *
 * Protection, per block:
 *   - MMU: the region's pages are all invalid until a block is allocated; the
 *     block's pages are mapped, the page right after it is never mapped
 *     (guard), and the pages are unmapped again when it is freed. An overrun
 *     past the end, an access to a free page or a use after free faults.
 *     Each operation checks this with AT S1E1R (mmu_hi_probe), which asks
 *     the MMU without touching the memory.
 *   - Canaries: the block ends exactly at its guard page; the bytes between
 *     the start of its first page and the data, and the < 16 bytes of
 *     alignment slack after the data, hold a known byte, checked on free.
 *   - Allocator: one mutex over the extent list, shared by the 4 cores.
 *   - Over memory: a request that does not fit is reported as OOM on the UART
 *     and retried at half the size (down to 1 MB, then waiting for frees);
 *     it is counted, not failed.
 *
 * Data: a 64-bit pattern derived from the address and a per-operation seed is
 * written, then read back and compared, then 256 random words are re-read.
 * Hardware writes and checks every word; QEMU one word per 4 KB page (the
 * same code, a stride of 8 bytes vs 4096), so the emulated run stays short.
 *
 * PASS: 600 operations done, no data/canary/MMU-probe error, every core ran
 * operations, nothing in use at the end, and the whole region unmapped.
 */

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <mmu_hi.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <test-smp-boot.hpp>
#include <test-console.hpp>

extern "C" unsigned port_cpu_id (void);
using namespace os::rtos;

namespace
{

constexpr unsigned kWorkers = 20;
constexpr unsigned kOps = 30;
constexpr std::uint64_t kMB = 1024ULL * 1024;
constexpr std::uint64_t kMinBytes = 1 * kMB;
constexpr std::uint64_t kMaxBytes = 512 * kMB;
constexpr std::uintptr_t kPage = 4096;
constexpr std::uintptr_t kHeadCanary = 64;
constexpr std::uint8_t kCanary = 0xC5;
constexpr unsigned kRandomReads = 256;
#if defined(QEMU_BUILD)
constexpr std::uintptr_t kStride = kPage;
#else
constexpr std::uintptr_t kStride = 8;
#endif

// ---------------------------------------------------------------------------
// The allocator: first fit over [g_lo, g_hi) in whole pages. Every extent is
// the block's pages plus one guard page after them. The bookkeeping lives
// here, never in the region (a free page is unmapped).

struct extent
{
  std::uintptr_t start;
  std::uintptr_t end; // the guard page included
};

mutex g_heap_mtx { "heap" };
extent g_live[kWorkers];
unsigned g_nlive = 0;
std::uintptr_t g_lo = 0, g_hi = 0;
std::uint64_t g_in_use = 0, g_peak = 0; // bytes asked for, live blocks

// Reserve 'need' bytes (a page multiple). 0 when nothing fits; then
// *largest is the biggest free gap.
std::uintptr_t
reserve (std::uintptr_t need, std::uint64_t size, std::uintptr_t* largest)
{
  g_heap_mtx.lock ();
  std::uintptr_t cursor = g_lo, at = 0, big = 0;
  unsigned pos = 0;
  for (unsigned i = 0; i <= g_nlive; ++i)
    {
      const std::uintptr_t gap_end = (i < g_nlive) ? g_live[i].start : g_hi;
      const std::uintptr_t gap = gap_end - cursor;
      if (gap > big)
        {
          big = gap;
        }
      if (gap >= need)
        {
          at = cursor;
          pos = i;
          break;
        }
      if (i < g_nlive)
        {
          cursor = g_live[i].end;
        }
    }
  if (at != 0)
    {
      for (unsigned j = g_nlive; j > pos; --j)
        {
          g_live[j] = g_live[j - 1];
        }
      g_live[pos] = { at, at + need };
      ++g_nlive;
      g_in_use += size;
      if (g_in_use > g_peak)
        {
          g_peak = g_in_use;
        }
    }
  *largest = big;
  g_heap_mtx.unlock ();
  return at;
}

void
release (std::uintptr_t start, std::uint64_t size)
{
  g_heap_mtx.lock ();
  for (unsigned i = 0; i < g_nlive; ++i)
    {
      if (g_live[i].start == start)
        {
          for (unsigned j = i; j + 1 < g_nlive; ++j)
            {
              g_live[j] = g_live[j + 1];
            }
          --g_nlive;
          g_in_use -= size;
          break;
        }
    }
  g_heap_mtx.unlock ();
}

std::uint64_t
in_use (void)
{
  g_heap_mtx.lock ();
  const std::uint64_t v = g_in_use;
  g_heap_mtx.unlock ();
  return v;
}

// ---------------------------------------------------------------------------

std::atomic<unsigned> g_ops_done { 0 };
std::atomic<unsigned> g_workers_done { 0 };
std::atomic<unsigned> g_oom { 0 };
std::atomic<unsigned> g_data_err { 0 };
std::atomic<unsigned> g_canary_err { 0 };
std::atomic<unsigned> g_probe_err { 0 };
std::atomic<std::uint64_t> g_bytes_alloc { 0 };
std::atomic<std::uint64_t> g_bytes_checked { 0 };
std::uint32_t g_core_ops[OS_NCPU] = { 0 };

// "123.45" MB, without floating point in printf.
const char*
mb (char* b, std::size_t n, std::uint64_t bytes)
{
  std::snprintf (b, n, "%lu.%02lu", static_cast<unsigned long> (bytes / kMB),
                 static_cast<unsigned long> ((bytes % kMB) * 100 / kMB));
  return b;
}

inline std::uint64_t
pattern (std::uintptr_t a, std::uint64_t seed)
{
  return (static_cast<std::uint64_t> (a) * 0x9E3779B97F4A7C15ULL) ^ seed;
}

inline std::uint64_t
next_rand (std::uint64_t& s)
{
  s ^= s >> 12;
  s ^= s << 25;
  s ^= s >> 27;
  return s * 0x2545F4914F6CDD1DULL;
}

void
expect_probe (unsigned id, unsigned op, const char* what, std::uintptr_t a,
              int want)
{
  if (mmu_hi_probe (a) != want)
    {
      ++g_probe_err;
      console ("[w%02u c%u] op %u MMU ERROR: %s 0x%08lx is %s\n", id,
               cpu_slot (), op, what, static_cast<unsigned long> (a),
               want ? "unmapped" : "mapped");
    }
}

void
data_error (unsigned id, unsigned op, std::uintptr_t a, std::uint64_t got,
            std::uint64_t want, unsigned& op_errors)
{
  ++op_errors;
  if (g_data_err++ < 20)
    {
      console ("[w%02u c%u] op %u DATA ERROR at 0x%08lx: 0x%016lx, want "
               "0x%016lx\n",
               id, cpu_slot (), op, static_cast<unsigned long> (a),
               static_cast<unsigned long> (got),
               static_cast<unsigned long> (want));
    }
}

void*
worker (void* arg)
{
  const unsigned id = static_cast<unsigned> (reinterpret_cast<std::uintptr_t> (arg));
  std::uint64_t rnd = 0x9E3779B97F4A7C15ULL * (id + 1)
                      ^ static_cast<std::uint64_t> (sysclock.now ());
  char b1[24], b2[24], b3[24];

  for (unsigned op = 1; op <= kOps; ++op)
    {
      std::uint64_t size = kMinBytes + next_rand (rnd) % (kMaxBytes - kMinBytes + 1);
      std::uintptr_t start = 0, pages = 0;

      // Over memory: halve the request until it fits, then wait for frees.
      for (;;)
        {
          pages = (size + kHeadCanary + kPage - 1) / kPage;
          std::uintptr_t largest = 0;
          start = reserve ((pages + 1) * kPage, size, &largest);
          if (start != 0)
            {
              break;
            }
          ++g_oom;
          const std::uint64_t smaller = (size / 2 > kMinBytes) ? size / 2 : kMinBytes;
          if (smaller != size)
            {
              console ("[w%02u c%u] op %2u/%u OOM: %s MB does not fit (in use "
                       "%s MB, largest free %s MB) -> retry %lu MB\n",
                       id, cpu_slot (), op, kOps, mb (b1, sizeof (b1), size),
                       mb (b2, sizeof (b2), in_use ()),
                       mb (b3, sizeof (b3), largest),
                       static_cast<unsigned long> (smaller / kMB));
              size = smaller;
            }
          sysclock.sleep_for (size == kMinBytes ? 10 : 1);
        }

      const std::uintptr_t map_end = start + pages * kPage;
      const std::uintptr_t len16 = (size + 15) & ~static_cast<std::uint64_t> (15);
      const std::uintptr_t user = map_end - len16;
      mmu_hi_map (start, pages * kPage, 1);
      g_bytes_alloc += size;

      console ("[w%02u c%u] op %2u/%u alloc %s MB @ 0x%08lx | in use %s / %s MB\n",
               id, cpu_slot (), op, kOps, mb (b1, sizeof (b1), size),
               static_cast<unsigned long> (user), mb (b2, sizeof (b2), in_use ()),
               mb (b3, sizeof (b3), g_hi - g_lo));

      expect_probe (id, op, "first byte", user, 1);
      expect_probe (id, op, "last byte", user + size - 1, 1);
      expect_probe (id, op, "guard page after", map_end, 0);
      expect_probe (id, op, "page before", start - kPage, 0);

      // Canaries around the data.
      auto* p8 = reinterpret_cast<volatile std::uint8_t*> (start);
      for (std::uintptr_t a = start; a < user; ++a)
        {
          p8[a - start] = kCanary;
        }
      for (std::uintptr_t a = user + size; a < map_end; ++a)
        {
          p8[a - start] = kCanary;
        }

      // Write, then read back.
      const std::uint64_t seed = next_rand (rnd);
      const std::uintptr_t words_end = user + (size & ~static_cast<std::uint64_t> (7));
      std::uint64_t checked = 0;
      unsigned op_errors = 0;
      for (std::uintptr_t a = user; a + 8 <= words_end; a += kStride)
        {
          *reinterpret_cast<volatile std::uint64_t*> (a) = pattern (a, seed);
        }
      if (words_end - user >= 8)
        {
          const std::uintptr_t last = words_end - 8;
          *reinterpret_cast<volatile std::uint64_t*> (last) = pattern (last, seed);
        }
      for (std::uintptr_t a = words_end; a < user + size; ++a)
        {
          *reinterpret_cast<volatile std::uint8_t*> (a) = static_cast<std::uint8_t> (seed ^ a);
        }

      for (std::uintptr_t a = user; a + 8 <= words_end; a += kStride)
        {
          const std::uint64_t v = *reinterpret_cast<volatile std::uint64_t*> (a);
          if (v != pattern (a, seed))
            {
              data_error (id, op, a, v, pattern (a, seed), op_errors);
            }
          checked += 8;
        }
      if (words_end - user >= 8)
        {
          const std::uintptr_t last = words_end - 8;
          const std::uint64_t v = *reinterpret_cast<volatile std::uint64_t*> (last);
          if (v != pattern (last, seed))
            {
              data_error (id, op, last, v, pattern (last, seed), op_errors);
            }
        }
      for (std::uintptr_t a = words_end; a < user + size; ++a)
        {
          const std::uint8_t v = *reinterpret_cast<volatile std::uint8_t*> (a);
          if (v != static_cast<std::uint8_t> (seed ^ a))
            {
              data_error (id, op, a, v, static_cast<std::uint8_t> (seed ^ a), op_errors);
            }
        }
      const std::uintptr_t slots = (words_end - user) / kStride;
      for (unsigned r = 0; r < kRandomReads && slots > 0; ++r)
        {
          const std::uintptr_t a = user + (next_rand (rnd) % slots) * kStride;
          const std::uint64_t v = *reinterpret_cast<volatile std::uint64_t*> (a);
          if (v != pattern (a, seed))
            {
              data_error (id, op, a, v, pattern (a, seed), op_errors);
            }
        }
      g_bytes_checked += checked;

      // Canaries intact?
      unsigned bad = 0;
      for (std::uintptr_t a = start; a < user; ++a)
        {
          bad += (p8[a - start] != kCanary);
        }
      for (std::uintptr_t a = user + size; a < map_end; ++a)
        {
          bad += (p8[a - start] != kCanary);
        }
      if (bad != 0)
        {
          ++g_canary_err;
          console ("[w%02u c%u] op %u CANARY ERROR: %u bytes changed\n", id,
                   cpu_slot (), op, bad);
        }

      // Free: unmap, check the block is gone, then give the pages back.
      mmu_hi_map (start, pages * kPage, 0);
      expect_probe (id, op, "freed first byte", user, 0);
      expect_probe (id, op, "freed last byte", user + size - 1, 0);
      release (start, size);

      console ("[w%02u c%u] op %2u/%u free  %s MB, data %s, canaries %s | in use %s MB\n",
               id, cpu_slot (), op, kOps, mb (b1, sizeof (b1), size),
               op_errors ? "ERR" : "ok", bad ? "ERR" : "ok",
               mb (b2, sizeof (b2), in_use ()));

      ++g_core_ops[cpu_slot ()];
      ++g_ops_done;
    }

  ++g_workers_done;
  return nullptr;
}

thread::stack::element_t s_stacks[kWorkers][2048];
char s_names[kWorkers][8];
thread* s_threads[kWorkers];

} // namespace

int
os_main (int, char*[])
{
  (void)os::rtos::interrupts::uncritical_section::enter ();
  console ("\n+=============================================================+\n");
  console ("|  " PORT_BANNER_SHORT " µOS++ SMP MEMORY TEST : smp_mem_test |\n");
  console ("+=============================================================+\n\n");

  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  const int waited = test_wait_secondaries (3000);
  console ("Secondary cores joined: %s (%d ms)\n", test_join_summary (), waited);

  std::uintptr_t base = 0, end = 0;
  mmu_hi_region (&base, &end);
  g_lo = base + kPage; // the region's first page stays unmapped
  g_hi = end;
  console ("Region 0x%08lx .. 0x%08lx: %lu MB, all pages unmapped\n",
           static_cast<unsigned long> (base), static_cast<unsigned long> (end),
           static_cast<unsigned long> ((end - base) / kMB));
  console ("%u threads x %u operations, blocks of 1 .. %lu MB, %s\n\n",
           kWorkers, kOps, static_cast<unsigned long> (kMaxBytes / kMB),
           (kStride == 8) ? "every word written and read back"
                          : "one word per 4 KB page written and read back");

  for (unsigned i = 0; i < kWorkers; ++i)
    {
      std::snprintf (s_names[i], sizeof (s_names[i]), "mw%02u", i);
      thread::attributes attr = thread::initializer;
      attr.th_stack_address = s_stacks[i];
      attr.th_stack_size_bytes = sizeof (s_stacks[i]);
      attr.th_priority = thread::priority::normal;
      s_threads[i] = new thread { s_names[i], worker,
                                  reinterpret_cast<void*> (static_cast<std::uintptr_t> (i)),
                                  attr };
      s_threads[i]->cpu_affinity (1u << (i % OS_NCPU));
    }

  char b1[24], b2[24];
  unsigned seconds = 0;
  while (g_workers_done.load () < kWorkers)
    {
      sysclock.sleep_for (1000);
      if (++seconds % 10 == 0)
        {
          console ("[t=%4us] ops %u/%u, in use %s MB, peak %s MB, OOM retries %u\n",
                   seconds, g_ops_done.load (), kWorkers * kOps,
                   mb (b1, sizeof (b1), in_use ()), mb (b2, sizeof (b2), g_peak),
                   g_oom.load ());
        }
    }

  // Every page of the region unmapped again (one probe per 2 MB and the ends).
  unsigned still_mapped = 0;
  for (std::uintptr_t a = base; a < end; a += 2 * kMB)
    {
      still_mapped += static_cast<unsigned> (mmu_hi_probe (a));
    }
  still_mapped += static_cast<unsigned> (mmu_hi_probe (end - kPage));

  bool all_cores = true;
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      all_cores = all_cores && (g_core_ops[c] > 0);
    }

  console ("\n==== smp_mem_test summary (%u s) ====\n", seconds);
  report_per_core ("operations per core", g_core_ops);
  console ("  operations      : %u / %u\n", g_ops_done.load (), kWorkers * kOps);
  console ("  allocated total : %s MB\n", mb (b1, sizeof (b1), g_bytes_alloc.load ()));
  console ("  written+checked : %s MB\n", mb (b1, sizeof (b1), g_bytes_checked.load ()));
  console ("  peak in use     : %s MB of %s MB\n", mb (b1, sizeof (b1), g_peak),
           mb (b2, sizeof (b2), g_hi - g_lo));
  console ("  OOM retries     : %u\n", g_oom.load ());
  console ("  data errors     : %u\n", g_data_err.load ());
  console ("  canary errors   : %u\n", g_canary_err.load ());
  console ("  MMU probe errors: %u\n", g_probe_err.load ());
  console ("  in use at end   : %s MB, pages still mapped: %u\n",
           mb (b1, sizeof (b1), in_use ()), still_mapped);

  const bool ok = test_secondaries_joined () && all_cores
                  && g_ops_done.load () == kWorkers * kOps
                  && g_data_err.load () == 0 && g_canary_err.load () == 0
                  && g_probe_err.load () == 0 && in_use () == 0
                  && still_mapped == 0;

  test::console_mutex ().lock ();
  uart::uart1.puts (ok ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");
  test::console_mutex ().unlock ();
  if (ok)
    {
      hw_result::ok ();
    }
  else
    {
      hw_result::fail ();
    }
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
}

// ----------------------------------------------------------------------------
// Startup trampolines (as smp_test3).
extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    led::init ();
    os_startup_initialize_free_store (
        __heap_start, static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    std::exit (os_main (0, nullptr));
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int
  main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*> (__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();

    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
    static thread main_thread {
      "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}
