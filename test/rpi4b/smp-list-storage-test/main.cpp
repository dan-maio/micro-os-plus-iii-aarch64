/*
 * smp-list-storage-test -- a generic intrusive list with its nodes in the
 * three storage regions (a thread's stack, the data segment and the heap),
 * shared by threads on all four cores. Console on the UART; no SD card.
 *
 * The C++ version of the Zig port's test/smp-list-storage-test (the
 * interfaces example's IntrusiveList), check for check:
 *
 * `intrusive_list<T>` is generic over any node type with a `T* next` member,
 * instantiated for two unrelated payloads: `record` (sealed: it detects a
 * field changed behind its back) and `text_node` (a 32-byte inline text
 * buffer). One list of each is shared by every thread, guarded by one µOS++
 * mutex. Every node is also checked to be where it claims: in its own
 * thread's stack (static arrays, `g_stacks`, in .bss), in the data-segment
 * arrays (with a non-zero initializer, so in .data) or in the free store
 * (the linker's .heap section, __heap_start .. __heap_end); the three ranges
 * do not overlap.
 *
 * Phase 1, regions: four threads, one per core, each put 8 records and one
 * text node on their stack, in the data segment and on the heap, push them
 * on the shared lists, and wait, their stack frames alive, while the main
 * thread checks the lists (96 records, 8 per thread and region, 12 texts,
 * every seal and address) and uppercases the data-segment texts through the
 * generic list. Released, each thread unlinks its stack nodes and returns;
 * the 64 data-segment and heap records and 8 texts stay.
 *
 * Phase 2, churn: 10 rounds of 1 s. Each round creates 8 workers (two per
 * core) and a scanner (no affinity). The workers push nodes from their stack
 * frame, from their data-segment pool and from the heap, unlink and free
 * them, change their values under the mutex and search the phase 1 records;
 * the scanner walks both lists under the mutex checking every seal and
 * address. At the end of the round each worker unlinks all its nodes and
 * returns; the nine threads are joined and deleted. After each round the
 * lists must again hold the phase 1 nodes, the data-segment pool must be
 * free and the heap back to its level.
 *
 * PASS when no check failed and, all nodes released, the heap is back to
 * its starting level. Only the verdict line goes to semihosting.
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <test-console.hpp>

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see this board's src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace os::rtos;

extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];
}

// ---- The generic intrusive list --------------------------------------------

template <typename T>
struct intrusive_list
{
  T* head = nullptr;

  void
  push (T* node)
  {
    node->next = head;
    head = node;
  }

  bool
  unlink (T* node)
  {
    T** link = &head;
    while (*link != nullptr)
      {
        if (*link == node)
          {
            *link = node->next;
            node->next = nullptr;
            return true;
          }
        link = &(*link)->next;
      }
    return false;
  }

  std::size_t
  count (void) const
  {
    std::size_t n = 0;
    for (T* it = head; it != nullptr; it = it->next)
      ++n;
    return n;
  }
};

enum class region : std::uint8_t
{
  stack,
  data,
  heap
};
static constexpr region regions[] = { region::stack, region::data, region::heap };
static constexpr unsigned n_regions = 3;

static const char*
region_name (region r)
{
  switch (r)
    {
    case region::stack:
      return "stack";
    case region::data:
      return "data";
    default:
      return "heap";
    }
}

// A record that can tell when its fields were changed behind its back.
struct record
{
  record* next = nullptr;
  std::uint32_t id = 0xD47A;
  std::uint8_t owner = 0;
  region where = region::data;
  std::uint64_t value = 0;
  std::uint64_t seal_word = 0;

  void
  init (std::uint32_t id_, std::uint8_t owner_, region where_, std::uint64_t value_)
  {
    next = nullptr;
    id = id_;
    owner = owner_;
    where = where_;
    value = value_;
    seal ();
  }

  std::uint64_t
  hash (void) const
  {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const std::uint64_t parts[] = { id, owner, static_cast<std::uint64_t> (where), value };
    for (auto x : parts)
      h = (h ^ x) * 0x00000100000001b3ull;
    return h;
  }

  void
  seal (void)
  {
    seal_word = hash ();
  }

  bool
  valid (void) const
  {
    return seal_word == hash ();
  }
};

// The interfaces example's StringNode.
struct text_node
{
  static constexpr std::size_t capacity = 32;

  text_node* next = nullptr;
  char text[capacity] = { '.' };
  std::size_t len = 1;
  region where = region::data;

  void
  init (const char* s, region where_)
  {
    next = nullptr;
    len = std::strlen (s);
    if (len > capacity)
      len = capacity;
    std::memcpy (text, s, len);
    where = where_;
  }

  void
  to_upper (void)
  {
    for (std::size_t i = 0; i < len; ++i)
      text[i] = static_cast<char> (std::toupper (static_cast<unsigned char> (text[i])));
  }
};

// ---- The three regions -------------------------------------------------------

static constexpr unsigned per_region = 8; // phase 1: records per thread and region
static constexpr unsigned rounds = 10;
static constexpr unsigned round_ms = 1000;
static constexpr unsigned n_workers = OS_NCPU * 2;
static constexpr unsigned pool = 6; // phase 2: nodes per worker and region
static constexpr std::size_t stack_elems = 4096;

// Every thread's stack: static, so its range is known and is neither the
// data-segment arrays nor the heap.
static thread::stack::element_t g_stacks[n_workers + 1][stack_elems] __attribute__ ((aligned (16)));

// The data segment: the default member initializers are not zero, so these
// arrays are constant-initialised into .data, not .bss.
static record g_data_recs[OS_NCPU][per_region];
static text_node g_data_txt[OS_NCPU];
static record g_pool_data[n_workers][pool];
static bool g_pool_busy[n_workers][pool];

template <typename A>
static bool
in_array (std::uintptr_t p, const A& arr)
{
  const auto base = reinterpret_cast<std::uintptr_t> (&arr);
  return p >= base && p < base + sizeof (arr);
}

static bool
region_of (const void* ptr, region* out)
{
  const auto p = reinterpret_cast<std::uintptr_t> (ptr);
  if (in_array (p, g_stacks))
    {
      *out = region::stack;
      return true;
    }
  if (in_array (p, g_data_recs) || in_array (p, g_data_txt) || in_array (p, g_pool_data))
    {
      *out = region::data;
      return true;
    }
  if (p >= reinterpret_cast<std::uintptr_t> (__heap_start)
      && p < reinterpret_cast<std::uintptr_t> (__heap_end))
    {
      *out = region::heap;
      return true;
    }
  return false;
}

static unsigned
stack_slot (const void* ptr)
{
  return static_cast<unsigned> ((reinterpret_cast<std::uintptr_t> (ptr)
                                 - reinterpret_cast<std::uintptr_t> (&g_stacks))
                                / sizeof (g_stacks[0]));
}

// ---- The shared lists ----------------------------------------------------------

static intrusive_list<record> g_recs;
static intrusive_list<text_node> g_texts;
static mutex g_mtx { "lists" };

static std::size_t
heap_bytes (void)
{
  return memory::get_default_resource ()->allocated_bytes ();
}

// ---- Checks ----------------------------------------------------------------

static std::atomic<std::uint32_t> g_errors { 0 };

#define BAD(fmt, ...)                                                          \
  do                                                                           \
    {                                                                          \
      if (g_errors.fetch_add (1) < 20)                                         \
        console ("  BAD c%u: " fmt "\n", cpu_slot (), ##__VA_ARGS__);          \
    }                                                                          \
  while (0)

static void
lock (void)
{
  if (g_mtx.lock () != result::ok)
    BAD ("list mutex lock");
}

static void
unlock (void)
{
  if (g_mtx.unlock () != result::ok)
    BAD ("list mutex unlock");
}

// The seal, and the address in the region the record claims (for a stack
// record, in its own thread's stack).
static bool
check_record (const record* r)
{
  if (!r->valid ())
    {
      BAD ("record %lx @%lx: broken seal", static_cast<unsigned long> (r->id),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (r)));
      return false;
    }
  region where;
  if (!region_of (r, &where))
    {
      BAD ("record %lx @%lx: in no known region", static_cast<unsigned long> (r->id),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (r)));
      return false;
    }
  if (where != r->where)
    {
      BAD ("record %lx: says %s, is in %s", static_cast<unsigned long> (r->id),
           region_name (r->where), region_name (where));
      return false;
    }
  if (where == region::stack && stack_slot (r) != r->owner)
    {
      BAD ("record %lx: on the stack of thread %u, owned by %u",
           static_cast<unsigned long> (r->id), stack_slot (r), r->owner);
      return false;
    }
  return true;
}

static bool
check_text (const text_node* t)
{
  region where;
  if (!region_of (t, &where) || where != t->where || t->len == 0
      || t->len > text_node::capacity)
    {
      BAD ("text @%lx (%s) in the wrong region or length %u",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (t)),
           region_name (t->where), static_cast<unsigned> (t->len));
      return false;
    }
  return true;
}

static std::uint32_t
id_of (unsigned owner, region r, unsigned i)
{
  return (owner << 16) | (static_cast<unsigned> (r) << 8) | i;
}

// ---- Threads ---------------------------------------------------------------

static char g_names[n_workers + 1][8];

static thread*
start_thread (unsigned slot, const char* prefix, unsigned n, thread::func_t func,
              std::uint32_t cpu_mask)
{
  std::snprintf (g_names[slot], sizeof (g_names[slot]), "%s%u", prefix, n);
  thread::attributes attr = thread::initializer;
  attr.th_stack_address = g_stacks[slot];
  attr.th_stack_size_bytes = sizeof (g_stacks[slot]);
  attr.th_cpu_affinity = cpu_mask;
  auto* th = new thread (g_names[slot], func,
                         reinterpret_cast<thread::func_args_t> (static_cast<std::uintptr_t> (slot)),
                         attr);
  if (th == nullptr)
    BAD ("cannot create thread %s", g_names[slot]);
  return th;
}

static void
join_and_delete (thread* th)
{
  if (th == nullptr)
    return;
  if (th->join () != result::ok)
    BAD ("join %s", th->name ());
  delete th;
}

// ---- Phase 1: one thread per core, nodes in the three regions ------------------

static semaphore_counting g_ready { "ready", OS_NCPU, 0 };
static semaphore* g_release[OS_NCPU];

static void*
region_worker (void* arg)
{
  const unsigned w = static_cast<unsigned> (reinterpret_cast<std::uintptr_t> (arg));
  const auto owner = static_cast<std::uint8_t> (w);

  // On this thread's stack: they live as long as this frame.
  record local[per_region];
  text_node local_txt;
  char buf[text_node::capacity + 1];

  for (unsigned i = 0; i < per_region; ++i)
    local[i].init (id_of (w, region::stack, i), owner, region::stack, 1000 * w + i);
  for (unsigned i = 0; i < per_region; ++i)
    g_data_recs[w][i].init (id_of (w, region::data, i), owner, region::data, 2000 * w + i);
  record* heap_recs[per_region] = {};
  for (unsigned i = 0; i < per_region; ++i)
    {
      record* r = new record;
      if (r == nullptr)
        {
          BAD ("w%u: out of memory", w);
          continue;
        }
      r->init (id_of (w, region::heap, i), owner, region::heap, 3000 * w + i);
      heap_recs[i] = r;
    }
  std::snprintf (buf, sizeof (buf), "w%u stack text", w);
  local_txt.init (buf, region::stack);
  std::snprintf (buf, sizeof (buf), "w%u data text", w);
  g_data_txt[w].init (buf, region::data);
  text_node* heap_txt = new text_node;
  if (heap_txt != nullptr)
    {
      std::snprintf (buf, sizeof (buf), "w%u heap text", w);
      heap_txt->init (buf, region::heap);
    }

  lock ();
  for (auto& r : local)
    g_recs.push (&r);
  for (auto& r : g_data_recs[w])
    g_recs.push (&r);
  for (auto* r : heap_recs)
    if (r != nullptr)
      g_recs.push (r);
  g_texts.push (&local_txt);
  g_texts.push (&g_data_txt[w]);
  if (heap_txt != nullptr)
    g_texts.push (heap_txt);
  unlock ();

  console ("  w%u c%u: 8 records + 1 text on its stack @%lx, in .data @%lx, on the heap @%lx\n",
           w, cpu_slot (), static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (local)),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (g_data_recs[w])),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (heap_recs[0])));

  g_ready.post ();
  g_release[w]->wait ();

  // The stack nodes die with this frame: out of the lists first.
  lock ();
  for (auto& r : local)
    if (!g_recs.unlink (&r))
      BAD ("w%u: stack record %lx not in the list", w, static_cast<unsigned long> (r.id));
  if (!g_texts.unlink (&local_txt))
    BAD ("w%u: stack text not in the list", w);
  unlock ();
  return nullptr;
}

static void
phase_regions (void)
{
  console ("\n-- phase 1: %u threads, %u records + 1 text each on the stack, in .data, on the heap\n",
           static_cast<unsigned> (OS_NCPU), per_region);
  static semaphore release_sems[OS_NCPU]; // binary, initially 0
  for (unsigned w = 0; w < OS_NCPU; ++w)
    g_release[w] = &release_sems[w];

  thread* th[OS_NCPU];
  for (unsigned w = 0; w < OS_NCPU; ++w)
    th[w] = start_thread (w, "reg", w, region_worker, 1u << w);
  for (unsigned w = 0; w < OS_NCPU; ++w)
    g_ready.wait ();

  // Every thread waits with its nodes in the lists, its stack alive.
  std::size_t tally[OS_NCPU][n_regions] = {};
  std::size_t bad_nodes = 0;
  lock ();
  const std::size_t n_recs = g_recs.count ();
  const std::size_t n_texts = g_texts.count ();
  for (record* r = g_recs.head; r != nullptr; r = r->next)
    {
      if (check_record (r) && r->owner < OS_NCPU)
        ++tally[r->owner][static_cast<unsigned> (r->where)];
      else
        ++bad_nodes;
    }
  // The generic list over the other payload: uppercase the .data texts.
  unsigned upper = 0;
  for (text_node* t = g_texts.head; t != nullptr; t = t->next)
    {
      if (!check_text (t))
        ++bad_nodes;
      if (t->where == region::data)
        {
          t->to_upper ();
          ++upper;
        }
    }
  bool texts_ok = true;
  for (text_node* t = g_texts.head; t != nullptr; t = t->next)
    {
      const bool want_upper = t->where == region::data;
      for (std::size_t i = 0; i < t->len; ++i)
        {
          const auto c = static_cast<unsigned char> (t->text[i]);
          if (std::isalpha (c) && (std::isupper (c) != 0) != want_upper)
            texts_ok = false;
        }
    }
  unlock ();

  for (unsigned w = 0; w < OS_NCPU; ++w)
    {
      console ("  w%u: stack %u, data %u, heap %u records\n", w,
               static_cast<unsigned> (tally[w][0]), static_cast<unsigned> (tally[w][1]),
               static_cast<unsigned> (tally[w][2]));
      for (auto n : tally[w])
        if (n != per_region)
          BAD ("w%u: %u records in a region, not %u", w, static_cast<unsigned> (n), per_region);
    }
  const std::size_t want_recs = OS_NCPU * n_regions * per_region;
  const std::size_t want_texts = OS_NCPU * n_regions;
  const bool lists_ok = n_recs == want_recs && n_texts == want_texts && bad_nodes == 0 && texts_ok;
  console ("  lists: %u records, %u texts (%u uppercased in .data) %s\n",
           static_cast<unsigned> (n_recs), static_cast<unsigned> (n_texts), upper,
           lists_ok ? "ok" : "BAD");
  if (n_recs != want_recs || n_texts != want_texts || bad_nodes != 0)
    BAD ("%u records, %u texts, %u bad nodes", static_cast<unsigned> (n_recs),
         static_cast<unsigned> (n_texts), static_cast<unsigned> (bad_nodes));
  if (!texts_ok)
    BAD ("texts not uppercased as asked");

  for (unsigned w = 0; w < OS_NCPU; ++w)
    g_release[w]->post ();
  for (auto* t : th)
    join_and_delete (t);

  std::size_t stack_left = 0;
  lock ();
  const std::size_t left_recs = g_recs.count ();
  const std::size_t left_texts = g_texts.count ();
  for (record* r = g_recs.head; r != nullptr; r = r->next)
    if (r->where == region::stack)
      ++stack_left;
  unlock ();
  const bool ok = left_recs == want_recs - OS_NCPU * per_region
                  && left_texts == want_texts - OS_NCPU && stack_left == 0;
  console ("  after the threads returned: %u records, %u texts, %u on a stack %s\n",
           static_cast<unsigned> (left_recs), static_cast<unsigned> (left_texts),
           static_cast<unsigned> (stack_left), ok ? "ok" : "BAD");
  if (!ok)
    BAD ("after phase 1: %u records, %u texts, %u stack nodes left",
         static_cast<unsigned> (left_recs), static_cast<unsigned> (left_texts),
         static_cast<unsigned> (stack_left));
}

// ---- Phase 2: churn --------------------------------------------------------------

static std::atomic<bool> g_stop { false };
static volatile std::uint32_t g_scans = 0;
static volatile unsigned g_round = 0;
static std::atomic<std::uint32_t> g_pushed { 0 };
static std::atomic<std::uint32_t> g_unlinked { 0 };

struct worker
{
  unsigned w;
  std::uint64_t rng;
  record* local; // [pool], on the worker's stack
  bool local_busy[pool] = {};
  record* heap[pool] = {};
  unsigned n_heap = 0;
  std::uint32_t next_id = 0;
  std::uint32_t pushed = 0;
  std::uint32_t unlinked = 0;
  std::uint32_t mutated = 0;
  std::uint32_t searched = 0;

  std::uint32_t
  rand (void)
  {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t> (rng >> 33);
  }

  void
  new_record (record* r, region where)
  {
    ++next_id;
    r->init (0x80000000u | (w << 20) | next_id, static_cast<std::uint8_t> (w), where, rand ());
    lock ();
    g_recs.push (r);
    unlock ();
    ++pushed;
  }

  void
  drop (record* r)
  {
    lock ();
    const bool ok = g_recs.unlink (r);
    unlock ();
    if (!ok)
      BAD ("w%u: own record %lx not in the list", w, static_cast<unsigned long> (r->id));
    check_record (r);
    ++unlinked;
  }

  // A slot of `busy` equal to `want`, starting from a random one; -1 if none.
  int
  pick (const bool* busy, bool want)
  {
    const unsigned start = rand () % pool;
    for (unsigned k = 0; k < pool; ++k)
      {
        const unsigned i = (start + k) % pool;
        if (busy[i] == want)
          return static_cast<int> (i);
      }
    return -1;
  }

  void
  push (region where)
  {
    if (where == region::stack)
      {
        const int i = pick (local_busy, false);
        if (i >= 0)
          {
            new_record (&local[i], region::stack);
            local_busy[i] = true;
          }
      }
    else if (where == region::data)
      {
        const int i = pick (g_pool_busy[w], false);
        if (i >= 0)
          {
            new_record (&g_pool_data[w][i], region::data);
            g_pool_busy[w][i] = true;
          }
      }
    else if (n_heap < pool)
      {
        record* r = new record;
        if (r == nullptr)
          {
            BAD ("w%u: out of memory", w);
            return;
          }
        new_record (r, region::heap);
        heap[n_heap++] = r;
      }
  }

  void
  release (region where)
  {
    if (where == region::stack)
      {
        const int i = pick (local_busy, true);
        if (i >= 0)
          {
            drop (&local[i]);
            local_busy[i] = false;
          }
      }
    else if (where == region::data)
      {
        const int i = pick (g_pool_busy[w], true);
        if (i >= 0)
          {
            drop (&g_pool_data[w][i]);
            g_pool_busy[w][i] = false;
          }
      }
    else if (n_heap > 0)
      {
        const unsigned i = rand () % n_heap;
        record* r = heap[i];
        drop (r);
        delete r;
        heap[i] = heap[--n_heap];
      }
  }

  void
  mutate (void)
  {
    record* r = nullptr;
    const int i = pick (local_busy, true);
    if (i >= 0)
      r = &local[i];
    else if (n_heap > 0)
      r = heap[0];
    else
      return;
    lock ();
    r->value += 1;
    r->seal ();
    unlock ();
    ++mutated;
  }

  void
  search (void)
  {
    // A phase 1 record: in .data or on the heap, they stay.
    const region where = (rand () & 1) == 0 ? region::data : region::heap;
    const std::uint32_t id = id_of (rand () % OS_NCPU, where, rand () % per_region);
    lock ();
    bool found = false;
    for (record* r = g_recs.head; r != nullptr; r = r->next)
      {
        if (r->id == id)
          {
            found = r->valid ();
            break;
          }
      }
    unlock ();
    if (!found)
      BAD ("w%u: phase 1 record %lx not found", w, static_cast<unsigned long> (id));
    ++searched;
  }
};

static void*
stress_worker (void* arg)
{
  const unsigned w = static_cast<unsigned> (reinterpret_cast<std::uintptr_t> (arg));
  record local[pool]; // the stack nodes
  worker self {};
  self.w = w;
  self.rng = 0x5EED0000ull + g_round * 977ull + w * 7919ull;
  self.local = local;
  std::uint32_t ops = 0;
  while (!g_stop.load ())
    {
      const unsigned op = self.rand () % 100;
      const region where = regions[self.rand () % n_regions];
      if (op < 50)
        self.push (where);
      else if (op < 85)
        self.release (where);
      else if (op < 95)
        self.mutate ();
      else
        self.search ();
      if ((++ops % 8) == 0)
        this_thread::yield ();
    }
  // Everything out, the stack nodes before this frame ends.
  for (auto where : regions)
    for (unsigned k = 0; k < pool; ++k)
      self.release (where);
  for (bool b : self.local_busy)
    if (b)
      BAD ("w%u: a stack node left", w);
  g_pushed.fetch_add (self.pushed);
  g_unlinked.fetch_add (self.unlinked);
  console ("  round %u w%u c%u: pushed %lu, unlinked %lu, changed %lu, searched %lu\n", g_round,
           w, cpu_slot (), static_cast<unsigned long> (self.pushed),
           static_cast<unsigned long> (self.unlinked), static_cast<unsigned long> (self.mutated),
           static_cast<unsigned long> (self.searched));
  return nullptr;
}

static void*
scanner_func (void*)
{
  const std::size_t limit = OS_NCPU * n_regions * per_region + n_workers * n_regions * pool;
  std::uint32_t scans = 0;
  while (!g_stop.load ())
    {
      lock ();
      std::size_t n = 0;
      for (record* r = g_recs.head; r != nullptr; r = r->next)
        {
          if (++n > limit)
            {
              BAD ("scan: more than %u records, a cycle?", static_cast<unsigned> (limit));
              break;
            }
          if (!check_record (r))
            break;
        }
      for (text_node* t = g_texts.head; t != nullptr; t = t->next)
        if (!check_text (t))
          break;
      unlock ();
      ++scans;
      sysclock.sleep_for (1);
    }
  g_scans = scans;
  return nullptr;
}

static void
phase_churn (void)
{
  console ("\n-- phase 2: %u rounds of %u workers + 1 scanner, %u ms each\n", rounds, n_workers,
           round_ms);
  lock ();
  const std::size_t base_recs = g_recs.count ();
  const std::size_t base_texts = g_texts.count ();
  unlock ();
  for (unsigned r = 0; r < rounds; ++r)
    {
      g_round = r;
      const std::size_t heap_before = heap_bytes ();
      const std::uint32_t pushed_before = g_pushed.load ();
      const std::uint32_t unlinked_before = g_unlinked.load ();
      g_stop.store (false);
      thread* th[n_workers];
      for (unsigned w = 0; w < n_workers; ++w)
        th[w] = start_thread (w, "w", w, stress_worker, 1u << (w % OS_NCPU));
      thread* scanner = start_thread (n_workers, "scan", r, scanner_func, 0xFFFFFFFFu);

      sysclock.sleep_for (round_ms);
      g_stop.store (true);
      for (auto* t : th)
        join_and_delete (t);
      join_and_delete (scanner);

      unsigned busy = 0;
      for (auto& row : g_pool_busy)
        for (bool b : row)
          if (b)
            ++busy;
      lock ();
      const std::size_t n = g_recs.count ();
      const std::size_t nt = g_texts.count ();
      unlock ();
      const std::size_t heap_after = heap_bytes ();
      const std::uint32_t pushed = g_pushed.load () - pushed_before;
      const std::uint32_t unlinked = g_unlinked.load () - unlinked_before;
      const bool ok = n == base_recs && nt == base_texts && busy == 0 && pushed == unlinked
                      && heap_after == heap_before;
      if (!ok)
        BAD ("round %u: %u records, %u texts, %u pool slots busy, +%lu -%lu, heap %u -> %u", r,
             static_cast<unsigned> (n), static_cast<unsigned> (nt), busy,
             static_cast<unsigned long> (pushed), static_cast<unsigned long> (unlinked),
             static_cast<unsigned> (heap_before), static_cast<unsigned> (heap_after));
      console ("  round %u: %u threads joined, +%lu -%lu nodes, %lu scans, lists %u/%u, heap %u B %s\n",
               r, n_workers + 1, static_cast<unsigned long> (pushed),
               static_cast<unsigned long> (unlinked), static_cast<unsigned long> (g_scans),
               static_cast<unsigned> (n), static_cast<unsigned> (nt),
               static_cast<unsigned> (heap_after), ok ? "ok" : "BAD");
    }
}

// Unlink every node; free the heap ones.
static void
release_all (void)
{
  lock ();
  while (record* r = g_recs.head)
    {
      g_recs.unlink (r);
      if (r->where == region::heap)
        delete r;
    }
  while (text_node* t = g_texts.head)
    {
      g_texts.unlink (t);
      if (t->where == region::heap)
        delete t;
    }
  unlock ();
}

// ---- Start-up (as every rpi4b test) --------------------------------------------

extern "C"
{
  void
  os_startup_initialize_hardware_early (void)
  {
  }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ smp-list-storage-test : " TEST_NCPU_STR
                   "-core intrusive list in 3 regions ==+\n\n";
    os_startup_initialize_free_store (__heap_start,
                                      static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    int code = os_main (0, nullptr);
    std::exit (code);
  }

  extern void
  os_startup_create_thread_idle (void);
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
    attr.th_cpu_affinity = 1u << 0;
    static thread main_thread { "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
                                nullptr, attr };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}

int
os_main (int, char*[])
{
  // Unmask IRQs before the scheduler starts. Portable across every port:
  // the architecture supplies the instruction, not the test.
  (void)os::rtos::interrupts::uncritical_section::enter ();

  console ("os_main on core %u\n", cpu_slot ());
  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  const int waited = test_wait_secondaries (3000);
  console ("SMP up (%s) after %d ms\n", test_join_summary (), waited);

  console ("  stacks  %016lx..%016lx\n",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (&g_stacks)),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (&g_stacks) + sizeof (g_stacks)));
  console ("  .data   %016lx..%016lx (records)\n",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (&g_data_recs)),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (&g_data_recs) + sizeof (g_data_recs)));
  console ("  heap    %016lx..%016lx\n",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (__heap_start)),
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (__heap_end)));

  const std::size_t heap_start = heap_bytes ();
  phase_regions ();
  phase_churn ();

  release_all ();
  const std::size_t heap_end = heap_bytes ();
  const bool heap_ok = heap_end == heap_start;
  console ("\nall nodes released, heap %u B (at start %u B) %s\n",
           static_cast<unsigned> (heap_end), static_cast<unsigned> (heap_start),
           heap_ok ? "ok" : "BAD");
  if (!heap_ok)
    BAD ("heap %u B at the end, %u B at the start", static_cast<unsigned> (heap_end),
         static_cast<unsigned> (heap_start));

  const std::uint32_t errors = g_errors.load ();
  if (errors == 0)
    {
      console ("RESULT: PASS (96 nodes in 3 regions checked, %u churn rounds, heap restored)\n",
               rounds);
      hw_result::ok ();
    }
  console ("RESULT: FAIL (%lu failed checks)\n", static_cast<unsigned long> (errors));
  hw_result::fail ();
  for (;;)
    sysclock.sleep_for (10000);
}
