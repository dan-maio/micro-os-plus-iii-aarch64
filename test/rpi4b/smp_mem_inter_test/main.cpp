/*
 * smp_mem_inter_test -- smp_mem_test plus inter-thread communication: each
 * block is filled by its owner, modified by another thread, and checked by
 * its owner again.
 *
 * 20 threads, 5 pinned on each of the 4 cores, 30 operations each. Thread i
 * works with thread (i + 1) % 20, in a ring. One operation of thread i:
 *
 *   1. allocate a block of 1 .. 512 MB (the RAM above the first GB, mapped
 *      page by page while the block is owned -- as smp_mem_test), write a
 *      pattern into it and compute its CRC32 (crc1);
 *   2. "write done": send the block (pointer, size, a random 64-bit mask) to
 *      thread i + 1 through its message queue, and lend it (state LENT);
 *   3. thread i + 1 checks the block is lent to it, XORs every word with the
 *      mask, and sends "modify done" back to thread i's queue;
 *   4. thread i checks the block came back modified, XORs it with the same
 *      mask again, computes the CRC32 (crc2) and prints crc1, crc2 and OK or
 *      NOT OK, then frees the block.
 *
 * Every thread is an owner (of its block) and a helper (of thread i - 1's):
 * while it waits -- for its "modify done", or for memory when over memory --
 * it keeps serving the requests in its queue, so the ring cannot deadlock.
 *
 * Protection:
 *   - between the threads: every block has a descriptor whose state moves
 *     OWNED -> LENT -> MODIFYING -> MODIFIED -> OWNED. Each side checks the
 *     state, the sender and the pointer/size it receives before touching the
 *     memory; anything else is a protocol error;
 *   - the memory: as smp_mem_test -- MMU guard page after each block, pages
 *     unmapped while free and after free (checked with AT S1E1R), canaries
 *     around the data, one mutex over the allocator, OOM retried at half size.
 *
 * The CRC is the A72's CRC32 instruction (__crc32d) over the words written.
 * Hardware writes, XORs and checks every word; QEMU one word per 4 KB page.
 *
 * PASS: 600 operations, crc1 == crc2 for all of them, no protocol, canary or
 * MMU-probe error, operations and modifications on every core, nothing in use
 * at the end and the whole region unmapped.
 */

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <led.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <mmu_hi.hpp>

#include <arm_acle.h>
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
#if defined(QEMU_BUILD)
constexpr std::uintptr_t kStride = kPage;
#else
constexpr std::uintptr_t kStride = 8;
#endif

// ---------------------------------------------------------------------------
// The allocator (as smp_mem_test): first fit over [g_lo, g_hi) in whole
// pages, each extent the block plus one guard page; bookkeeping outside the
// region.

struct extent
{
  std::uintptr_t start;
  std::uintptr_t end;
};

mutex g_heap_mtx { "heap" };
extent g_live[kWorkers];
unsigned g_nlive = 0;
std::uintptr_t g_lo = 0, g_hi = 0;
std::uint64_t g_in_use = 0, g_peak = 0;

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
// The blocks and the messages between the threads.

enum : unsigned
{
  kOwned = 0,
  kLent,      // sent to the helper, the owner does not touch it
  kModifying, // the helper is XORing it
  kModified   // the helper is done, back to the owner
};

struct block
{
  std::atomic<unsigned> state { kOwned };
  std::uintptr_t user = 0;
  std::uint64_t size = 0;
  unsigned helper = 0;
};

enum : std::uint32_t
{
  kMsgModify = 1, // owner -> helper: "write done, modify this block"
  kMsgDone = 2    // helper -> owner: "modify done"
};

struct msg
{
  std::uint32_t type;
  std::uint32_t from;
  std::uint32_t op;
  std::uint32_t pad;
  std::uintptr_t user;
  std::uint64_t size;
  std::uint64_t mask;
};

block g_block[kWorkers];
message_queue* g_q[kWorkers];

std::atomic<unsigned> g_ops_done { 0 };
std::atomic<unsigned> g_workers_done { 0 };
std::atomic<unsigned> g_oom { 0 };
std::atomic<unsigned> g_crc_ok { 0 };
std::atomic<unsigned> g_crc_bad { 0 };
std::atomic<unsigned> g_proto_err { 0 };
std::atomic<unsigned> g_canary_err { 0 };
std::atomic<unsigned> g_probe_err { 0 };
std::atomic<std::uint64_t> g_bytes_alloc { 0 };
std::uint32_t g_core_ops[OS_NCPU] = { 0 };
std::uint32_t g_core_mods[OS_NCPU] = { 0 };

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

// The words of a block the test touches: every kStride bytes, plus the last
// whole word, plus the trailing bytes.
inline std::uintptr_t
words_end (std::uintptr_t user, std::uint64_t size)
{
  return user + (size & ~static_cast<std::uint64_t> (7));
}

std::uint32_t
block_crc (std::uintptr_t user, std::uint64_t size)
{
  const std::uintptr_t we = words_end (user, size);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uintptr_t a = user; a + 8 <= we; a += kStride)
    {
      crc = __crc32d (crc, *reinterpret_cast<volatile std::uint64_t*> (a));
    }
  if (we - user >= 8)
    {
      crc = __crc32d (crc, *reinterpret_cast<volatile std::uint64_t*> (we - 8));
    }
  for (std::uintptr_t a = we; a < user + size; ++a)
    {
      crc = __crc32b (crc, *reinterpret_cast<volatile std::uint8_t*> (a));
    }
  return ~crc;
}

void
block_xor (std::uintptr_t user, std::uint64_t size, std::uint64_t mask)
{
  const std::uintptr_t we = words_end (user, size);
  for (std::uintptr_t a = user; a + 8 <= we; a += kStride)
    {
      *reinterpret_cast<volatile std::uint64_t*> (a) ^= mask;
    }
  // The last whole word, unless the strided loop already reached it.
  if (we - user >= 8 && ((we - 8 - user) % kStride) != 0)
    {
      *reinterpret_cast<volatile std::uint64_t*> (we - 8) ^= mask;
    }
  for (std::uintptr_t a = we; a < user + size; ++a)
    {
      *reinterpret_cast<volatile std::uint8_t*> (a) ^= static_cast<std::uint8_t> (mask);
    }
}

void
proto_error (unsigned id, const char* what, unsigned other)
{
  ++g_proto_err;
  console ("[w%02u c%u] PROTOCOL ERROR: %s (w%02u)\n", id, cpu_slot (), what, other);
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

// Act as the helper for one "modify" request.
void
serve_modify (unsigned id, const msg& m)
{
  const unsigned owner = m.from;
  block& b = g_block[owner];
  unsigned expected = kLent;
  if (owner >= kWorkers || b.helper != id || b.user != m.user || b.size != m.size
      || !b.state.compare_exchange_strong (expected, kModifying))
    {
      proto_error (id, "modify request for a block not lent to me", owner);
      return;
    }
  block_xor (m.user, m.size, m.mask);
  ++g_core_mods[cpu_slot ()];
  char b1[24];
  console ("[w%02u c%u]        xor w%02u op %2u: %s MB @ 0x%08lx mask 0x%016lx -> done\n",
           id, cpu_slot (), owner, m.op, mb (b1, sizeof (b1), m.size),
           static_cast<unsigned long> (m.user), static_cast<unsigned long> (m.mask));
  b.state.store (kModified);
  msg d = { kMsgDone, id, m.op, 0, m.user, m.size, m.mask };
  g_q[owner]->send (&d, sizeof (d));
}

// Wait up to 'ms' for one message and handle it. True when it was the
// "modify done" for this owner's operation 'op'.
bool
serve_once (unsigned id, unsigned op, clock::duration_t ms)
{
  msg m;
  if (g_q[id]->timed_receive (&m, sizeof (m), ms) != result::ok)
    {
      return false;
    }
  if (m.type == kMsgModify)
    {
      serve_modify (id, m);
      return false;
    }
  if (m.type == kMsgDone && m.op == op && m.from == g_block[id].helper)
    {
      return true;
    }
  proto_error (id, "unexpected message", m.from);
  return false;
}

void*
worker (void* arg)
{
  const unsigned id = static_cast<unsigned> (reinterpret_cast<std::uintptr_t> (arg));
  const unsigned helper = (id + 1) % kWorkers;
  std::uint64_t rnd = 0x9E3779B97F4A7C15ULL * (id + 1)
                      ^ static_cast<std::uint64_t> (sysclock.now ());
  char b1[24], b2[24], b3[24];
  block& blk = g_block[id];
  blk.helper = helper;

  for (unsigned op = 1; op <= kOps; ++op)
    {
      std::uint64_t size = kMinBytes + next_rand (rnd) % (kMaxBytes - kMinBytes + 1);
      std::uintptr_t start = 0, pages = 0;

      // Over memory: halve the request until it fits; while waiting, keep
      // serving the ring.
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
          serve_once (id, 0, size == kMinBytes ? 10 : 1);
        }

      const std::uintptr_t map_end = start + pages * kPage;
      const std::uintptr_t len16 = (size + 15) & ~static_cast<std::uint64_t> (15);
      const std::uintptr_t user = map_end - len16;
      mmu_hi_map (start, pages * kPage, 1);
      g_bytes_alloc += size;

      expect_probe (id, op, "first byte", user, 1);
      expect_probe (id, op, "last byte", user + size - 1, 1);
      expect_probe (id, op, "guard page after", map_end, 0);
      expect_probe (id, op, "page before", start - kPage, 0);

      auto* p8 = reinterpret_cast<volatile std::uint8_t*> (start);
      for (std::uintptr_t a = start; a < user; ++a)
        {
          p8[a - start] = kCanary;
        }
      for (std::uintptr_t a = user + size; a < map_end; ++a)
        {
          p8[a - start] = kCanary;
        }

      // 1. Write the pattern, compute crc1.
      const std::uint64_t seed = next_rand (rnd);
      const std::uintptr_t we = words_end (user, size);
      for (std::uintptr_t a = user; a + 8 <= we; a += kStride)
        {
          *reinterpret_cast<volatile std::uint64_t*> (a) = pattern (a, seed);
        }
      if (we - user >= 8)
        {
          *reinterpret_cast<volatile std::uint64_t*> (we - 8) = pattern (we - 8, seed);
        }
      for (std::uintptr_t a = we; a < user + size; ++a)
        {
          *reinterpret_cast<volatile std::uint8_t*> (a) = static_cast<std::uint8_t> (seed ^ a);
        }
      const std::uint32_t crc1 = block_crc (user, size);
      std::uint64_t mask = next_rand (rnd);
      if (mask == 0)
        {
          mask = 0x5A5A5A5A5A5A5A5AULL;
        }

      // 2. "Write done": lend the block to the helper.
      blk.user = user;
      blk.size = size;
      blk.state.store (kLent);
      console ("[w%02u c%u] op %2u/%u alloc %s MB @ 0x%08lx, crc1 0x%08x -> w%02u "
               "| in use %s / %s MB\n",
               id, cpu_slot (), op, kOps, mb (b1, sizeof (b1), size),
               static_cast<unsigned long> (user), crc1, helper,
               mb (b2, sizeof (b2), in_use ()), mb (b3, sizeof (b3), g_hi - g_lo));
      msg m = { kMsgModify, id, op, 0, user, size, mask };
      g_q[helper]->send (&m, sizeof (m));

      // 3. Wait for "modify done", serving the ring meanwhile.
      while (!serve_once (id, op, 10))
        {
        }

      unsigned expected = kModified;
      if (!blk.state.compare_exchange_strong (expected, kOwned))
        {
          proto_error (id, "block came back not modified", helper);
          blk.state.store (kOwned);
        }

      // 4. XOR back, crc2, compare.
      block_xor (user, size, mask);
      const std::uint32_t crc2 = block_crc (user, size);
      const bool ok = (crc1 == crc2);
      ++(ok ? g_crc_ok : g_crc_bad);

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

      mmu_hi_map (start, pages * kPage, 0);
      expect_probe (id, op, "freed first byte", user, 0);
      expect_probe (id, op, "freed last byte", user + size - 1, 0);
      release (start, size);

      console ("[w%02u c%u] op %2u/%u crc1 0x%08x crc2 0x%08x %s, free %s MB | in use %s MB\n",
               id, cpu_slot (), op, kOps, crc1, crc2, ok ? "OK" : "NOT OK",
               mb (b1, sizeof (b1), size), mb (b2, sizeof (b2), in_use ()));

      ++g_core_ops[cpu_slot ()];
      ++g_ops_done;
    }

  // Done as an owner; stay a helper until every thread is done.
  ++g_workers_done;
  while (g_workers_done.load () < kWorkers)
    {
      serve_once (id, 0, 10);
    }
  return nullptr;
}

thread::stack::element_t s_stacks[kWorkers][2048];
char s_names[kWorkers][8];
char s_qnames[kWorkers][8];
thread* s_threads[kWorkers];

} // namespace

int
os_main (int, char*[])
{
  (void)os::rtos::interrupts::uncritical_section::enter ();
  console ("\n+=============================================================+\n");
  console ("|  " PORT_BANNER_SHORT " µOS++ SMP MEMORY + INTER-THREAD TEST     |\n");
  console ("|  smp_mem_inter_test                                          |\n");
  console ("+=============================================================+\n\n");

  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  const int waited = test_wait_secondaries (3000);
  console ("Secondary cores joined: %s (%d ms)\n", test_join_summary (), waited);

  std::uintptr_t base = 0, end = 0;
  mmu_hi_region (&base, &end);
  g_lo = base + kPage;
  g_hi = end;
  console ("Region 0x%08lx .. 0x%08lx: %lu MB, all pages unmapped\n",
           static_cast<unsigned long> (base), static_cast<unsigned long> (end),
           static_cast<unsigned long> ((end - base) / kMB));
  console ("%u threads in a ring (w[i] -> w[i+1]) x %u operations, blocks of "
           "1 .. %lu MB, %s\n\n",
           kWorkers, kOps, static_cast<unsigned long> (kMaxBytes / kMB),
           (kStride == 8) ? "every word written, XORed and CRCed"
                          : "one word per 4 KB page written, XORed and CRCed");

  for (unsigned i = 0; i < kWorkers; ++i)
    {
      std::snprintf (s_qnames[i], sizeof (s_qnames[i]), "mq%02u", i);
      g_q[i] = new message_queue { s_qnames[i], 4, sizeof (msg) };
    }
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
          console ("[t=%4us] ops %u/%u, crc OK %u NOT OK %u, in use %s MB, peak %s MB, "
                   "OOM retries %u\n",
                   seconds, g_ops_done.load (), kWorkers * kOps, g_crc_ok.load (),
                   g_crc_bad.load (), mb (b1, sizeof (b1), in_use ()),
                   mb (b2, sizeof (b2), g_peak), g_oom.load ());
        }
    }
  sysclock.sleep_for (50); // let the last helpers leave their wait

  unsigned still_mapped = 0;
  for (std::uintptr_t a = base; a < end; a += 2 * kMB)
    {
      still_mapped += static_cast<unsigned> (mmu_hi_probe (a));
    }
  still_mapped += static_cast<unsigned> (mmu_hi_probe (end - kPage));

  bool all_cores = true;
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      all_cores = all_cores && (g_core_ops[c] > 0) && (g_core_mods[c] > 0);
    }

  console ("\n==== smp_mem_inter_test summary (%u s) ====\n", seconds);
  report_per_core ("operations per core   ", g_core_ops);
  report_per_core ("modifications per core", g_core_mods);
  console ("  operations      : %u / %u\n", g_ops_done.load (), kWorkers * kOps);
  console ("  crc1 == crc2    : %u OK, %u NOT OK\n", g_crc_ok.load (), g_crc_bad.load ());
  console ("  allocated total : %s MB\n", mb (b1, sizeof (b1), g_bytes_alloc.load ()));
  console ("  peak in use     : %s MB of %s MB\n", mb (b1, sizeof (b1), g_peak),
           mb (b2, sizeof (b2), g_hi - g_lo));
  console ("  OOM retries     : %u\n", g_oom.load ());
  console ("  protocol errors : %u\n", g_proto_err.load ());
  console ("  canary errors   : %u\n", g_canary_err.load ());
  console ("  MMU probe errors: %u\n", g_probe_err.load ());
  console ("  in use at end   : %s MB, pages still mapped: %u\n",
           mb (b1, sizeof (b1), in_use ()), still_mapped);

  const bool ok = test_secondaries_joined () && all_cores
                  && g_ops_done.load () == kWorkers * kOps
                  && g_crc_ok.load () == kWorkers * kOps && g_crc_bad.load () == 0
                  && g_proto_err.load () == 0 && g_canary_err.load () == 0
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
