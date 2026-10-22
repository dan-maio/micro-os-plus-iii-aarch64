/*
 * smp-shapes-test -- the interfaces example (intrusive generic list,
 * polymorphism, threads) on µOS++, on all four cores, with its output on
 * the UART and on the SD card.
 *
 * The C++ version of the Zig port's test/smp-shapes-test, check for check,
 * with real C++ polymorphism where the Zig port builds its vtable by hand:
 * `shape` is an abstract base class (virtual area(), name(), get_kind() and
 * do_scale(), a virtual destructor), and `circle`, `triangle`, `ellipse`
 * and `rect` derive from it, dispatched through the compiler's vtables. The
 * intrusive `next` link is a member of the base. Each shape also records the
 * area its constructor arguments (and every scale since) predict. The image
 * is built with -fno-rtti, so the kind is a virtual function, not a
 * dynamic_cast; and since a vtable pointer cannot be checked portably, the
 * base carries a magic word, set by its constructor and cleared by its
 * destructor, so a corrupted or already freed node is reported, never
 * deleted. A shape is freed with `delete` through the base (the virtual
 * destructor). One `intrusive_list<shape>` is shared by every thread,
 * guarded by one µOS++ mutex; every shape lives on the free store.
 *
 * Phase 1, build: four builder threads, one per kind and one per core, each
 * construct 10 shapes, check each one through its virtual functions (magic
 * word, kind, name, area) and push it on the list. Then the list is
 * checked: 40 nodes, 10 per kind, the total area, a search per kind.
 *
 * Phase 1b, scale: four threads, one per kind, scale their kind by 2 under
 * the mutex at the same time, check the kind's area grew 4x, scale it back;
 * modify() (search, then scale) x3 and back.
 *
 * Phase 2, stress: 20 rounds of 1.5 s. Each round creates 8 workers (two per
 * core) and a scanner; the workers create, check, push, scale, search,
 * unlink and free shapes of random kinds and sizes (each changes only its
 * own), the scanner walks the list under the mutex checking every node. At
 * the end of the round every worker frees its shapes and returns; the nine
 * threads are joined and deleted. After each round the list must hold the 40
 * phase 1 shapes with their total area, and the heap must be back to its
 * level before the round.
 *
 * Output: one logger thread on core 0 owns the UART and the SD card; every
 * other thread sends it lines through a message queue. It prints each line
 * on the UART and appends it to tests/shapes.log on the card's existing
 * FAT32 partition (never formatted). At the end the file is read back, its
 * size and CRC32 compared with what was written. (The QEMU image has no SD
 * card: there the lines go to the UART only and the read-back is skipped.)
 *
 * PASS when no check failed, every created shape was freed, the heap is
 * back to its starting level and the SD log reads back intact. Only the
 * verdict line goes to semihosting.
 */
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <uart.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>
#include <hw_result.hpp>
#include <test-console.hpp>
#include <sd.hpp>
#if defined(HW_BUILD)
#include <fatfs_hw.hpp>
#endif

// Secondary-core idle stacks, the idle body and smp_install_boot_threads()
// are identical in every SMP test; see this board's src/test-smp-boot.cpp.
#include <test-smp-boot.hpp>

#include <atomic>
#include <cmath>
#include <cstdarg>
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

static constexpr double pi = 3.14159265358979323846;

// ---- The generic intrusive list and the shape interface --------------------

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

enum class kind : std::uint8_t
{
  circle,
  triangle,
  ellipse,
  rect
};
static constexpr kind kinds[] = { kind::circle, kind::triangle, kind::ellipse, kind::rect };
static constexpr unsigned n_kinds = 4;

static const char*
kind_name (kind k)
{
  static const char* const names[] = { "circle", "triangle", "ellipse", "rect" };
  return names[static_cast<unsigned> (k)];
}

// The interface: an abstract base class, dispatched through the vtable the
// compiler builds. The intrusive link is the base's own `next`. The image is
// built with -fno-rtti, so the concrete kind is a virtual function too.
class shape
{
public:
  shape* next = nullptr;

  virtual ~shape ()
  {
    magic_ = 0; // a node used after delete shows as a broken magic word
  }

  virtual double
  area (void) const
      = 0;
  virtual const char*
  name (void) const
      = 0;
  virtual kind
  get_kind (void) const
      = 0;

  // Scale the dimensions; the predicted area follows (factor squared).
  void
  scale (double factor)
  {
    do_scale (factor);
    expected_ *= factor * factor;
  }

  // The area the constructor arguments (and every scale since) predict.
  double
  expected (void) const
  {
    return expected_;
  }

  // Set by the constructor, cleared by the destructor: false for a corrupted
  // or already freed node.
  bool
  alive (void) const
  {
    return magic_ == alive_magic;
  }

protected:
  explicit shape (double expected) : expected_ { expected }
  {
  }

  virtual void
  do_scale (double factor)
      = 0;

private:
  static constexpr std::uint32_t alive_magic = 0x5A4E3D21u;
  std::uint32_t magic_ = alive_magic;
  double expected_;
};

class circle final : public shape
{
public:
  explicit circle (double r) : shape { pi * r * r }, radius_ { r }
  {
  }

  double
  area (void) const override
  {
    return pi * radius_ * radius_;
  }
  const char*
  name (void) const override
  {
    return "circle";
  }
  kind
  get_kind (void) const override
  {
    return kind::circle;
  }

private:
  void
  do_scale (double f) override
  {
    radius_ *= f;
  }

  double radius_;
};

class rect final : public shape
{
public:
  rect (double w, double h) : shape { w * h }, w_ { w }, h_ { h }
  {
  }

  double
  area (void) const override
  {
    return w_ * h_;
  }
  const char*
  name (void) const override
  {
    return "rect";
  }
  kind
  get_kind (void) const override
  {
    return kind::rect;
  }

private:
  void
  do_scale (double f) override
  {
    w_ *= f;
    h_ *= f;
  }

  double w_, h_;
};

class triangle final : public shape
{
public:
  triangle (double base, double height) : shape { 0.5 * base * height }, base_ { base }, height_ { height }
  {
  }

  double
  area (void) const override
  {
    return 0.5 * base_ * height_;
  }
  const char*
  name (void) const override
  {
    return "triangle";
  }
  kind
  get_kind (void) const override
  {
    return kind::triangle;
  }

private:
  void
  do_scale (double f) override
  {
    base_ *= f;
    height_ *= f;
  }

  double base_, height_;
};

class ellipse final : public shape
{
public:
  ellipse (double a, double b) : shape { pi * a * b }, semi_major_ { a }, semi_minor_ { b }
  {
  }

  double
  area (void) const override
  {
    return pi * semi_major_ * semi_minor_;
  }
  const char*
  name (void) const override
  {
    return "ellipse";
  }
  kind
  get_kind (void) const override
  {
    return kind::ellipse;
  }

private:
  void
  do_scale (double f) override
  {
    semi_major_ *= f;
    semi_minor_ *= f;
  }

  double semi_major_, semi_minor_;
};

struct shape_list
{
  intrusive_list<shape> nodes;

  void
  push (shape* s)
  {
    nodes.push (s);
  }

  shape*
  search (const char* name) const
  {
    for (shape* s = nodes.head; s != nullptr; s = s->next)
      if (std::strcmp (s->name (), name) == 0)
        return s;
    return nullptr;
  }

  bool
  modify (const char* name, double factor) const
  {
    shape* s = search (name);
    if (s == nullptr)
      return false;
    s->scale (factor);
    return true;
  }

  bool
  remove (shape* s)
  {
    return nodes.unlink (s);
  }

  double
  total_area (void) const
  {
    double sum = 0;
    for (shape* s = nodes.head; s != nullptr; s = s->next)
      sum += s->area ();
    return sum;
  }

  std::size_t
  count (void) const
  {
    return nodes.count ();
  }
};

// One shape of `k` on the heap from two parameters (a circle uses `a`).
static shape*
make (kind k, double a, double b)
{
  switch (k)
    {
    case kind::circle:
      return new circle (a);
    case kind::triangle:
      return new triangle (a, b);
    case kind::ellipse:
      return new ellipse (a, b);
    default:
      return new rect (a, b);
    }
}

static bool
near_eq (double got, double want)
{
  const double scale = std::fabs (want) > 1.0 ? std::fabs (want) : 1.0;
  return std::fabs (got - want) <= 1e-9 * scale;
}

// ---- Constants -------------------------------------------------------------

static constexpr unsigned per_kind = 10;
static constexpr unsigned rounds = 20;
static constexpr unsigned round_ms = 1500;
static constexpr unsigned n_workers = OS_NCPU * 2;
static constexpr unsigned max_owned = 16;
static constexpr std::size_t thread_stack_bytes = 32 * 1024;
static const char* const log_file = "shapes.log";

// ---- The logger: core 0 owns the UART and the SD card ----------------------

enum class log_op : std::uint8_t
{
  line,
  flush,
  stop
};

struct log_msg
{
  log_op op = log_op::line;
  std::uint8_t len = 0;
  char text[110];
};

static message_queue_inclusive<log_msg, 64> g_log_q { "log" };
static semaphore_binary g_log_ack { "log_ack", 0 };

static char g_sd_buf[4096];
static std::size_t g_sd_len = 0;
static std::size_t g_sd_bytes = 0;
static std::uint32_t g_sd_crc = 0; // CRC-32 (IEEE), running, pre-inverted
static std::uint32_t g_sd_errors = 0;

static std::uint32_t
crc32_update (std::uint32_t crc, const void* data, std::size_t len)
{
  crc = ~crc;
  const auto* p = static_cast<const std::uint8_t*> (data);
  for (std::size_t i = 0; i < len; ++i)
    {
      crc ^= p[i];
      for (int k = 0; k < 8; ++k)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  return ~crc;
}

// A double as fixed-point text, `decimals` digits after the point, for %s.
//
// The test never formats a floating-point value with printf's %f: newlib's
// conversion (dtoa) keeps its bignum free list in one reentrancy structure
// shared by every core here, so %f on several cores at once corrupted it
// (wrong digits, then a hang, seen under QEMU), and it allocates bignums from
// the free store the first time it needs each size and never returns them
// (the heap ended above its starting level). Integer formatting has neither.
struct fixed_text
{
  char s[32];
};

static fixed_text
fx (double v, unsigned decimals)
{
  fixed_text t;
  if (v != v)
    {
      std::snprintf (t.s, sizeof (t.s), "nan");
      return t;
    }
  const bool neg = v < 0;
  if (neg)
    v = -v;
  std::uint64_t scale = 1;
  for (unsigned i = 0; i < decimals; ++i)
    scale *= 10;
  if (v >= 1.0e15)
    {
      std::snprintf (t.s, sizeof (t.s), "%sbig", neg ? "-" : "");
      return t;
    }
  const auto scaled = static_cast<std::uint64_t> (v * static_cast<double> (scale) + 0.5);
  const std::uint64_t whole = scaled / scale;
  const std::uint64_t frac = scaled % scale;
  if (decimals == 0)
    std::snprintf (t.s, sizeof (t.s), "%s%llu", neg ? "-" : "",
                   static_cast<unsigned long long> (whole));
  else
    std::snprintf (t.s, sizeof (t.s), "%s%llu.%0*llu", neg ? "-" : "",
                   static_cast<unsigned long long> (whole), static_cast<int> (decimals),
                   static_cast<unsigned long long> (frac));
  return t;
}

// Format one line and queue it for the logger (blocks while the queue is
// full). Lines longer than the message are cut.
static void
say (const char* fmt, ...)
{
  log_msg m;
  std::va_list args;
  va_start (args, fmt);
  int n = std::vsnprintf (m.text, sizeof (m.text), fmt, args);
  va_end (args);
  if (n < 0)
    n = 0;
  if (n > static_cast<int> (sizeof (m.text)) - 1)
    n = static_cast<int> (sizeof (m.text)) - 1;
  m.len = static_cast<std::uint8_t> (n);
  g_log_q.send (&m);
}

// Ask the logger to write what it holds to the card, and wait until it has.
static void
log_sync (log_op op)
{
  log_msg m;
  m.op = op;
  g_log_q.send (&m);
  g_log_ack.wait ();
}

static void
sd_flush (void)
{
  if (g_sd_len == 0)
    return;
#if defined(HW_BUILD)
  if (fatfshw::append_file (log_file, g_sd_buf, static_cast<std::uint32_t> (g_sd_len)))
    {
      g_sd_bytes += g_sd_len;
      g_sd_crc = crc32_update (g_sd_crc, g_sd_buf, g_sd_len);
    }
  else
    {
      ++g_sd_errors;
    }
#endif
  g_sd_len = 0;
}

static void*
logger_func (void*)
{
  for (;;)
    {
      log_msg m;
      // A quiet half second writes out what is buffered.
      if (g_log_q.timed_receive (&m, 500) != result::ok)
        {
          sd_flush ();
          continue;
        }
      if (m.op == log_op::line)
        {
          m.text[m.len] = '\0';
          uart::uart1.puts (m.text);
          if (g_sd_len + m.len > sizeof (g_sd_buf))
            sd_flush ();
          std::memcpy (g_sd_buf + g_sd_len, m.text, m.len);
          g_sd_len += m.len;
        }
      else
        {
          sd_flush ();
          g_log_ack.post ();
          if (m.op == log_op::stop)
            return nullptr;
        }
    }
}

// ---- Checks ----------------------------------------------------------------

static std::atomic<std::uint32_t> g_errors { 0 };

#define BAD(fmt, ...)                                                          \
  do                                                                           \
    {                                                                          \
      if (g_errors.fetch_add (1) < 20)                                         \
        say ("  BAD c%u: " fmt "\n", cpu_slot (), ##__VA_ARGS__);              \
    }                                                                          \
  while (0)

// The checks every freshly built or still listed shape must pass; `want`
// is the kind it must be, or nullptr for any.
static bool
check_shape (const shape* s, const kind* want)
{
  if (!s->alive ())
    {
      BAD ("node @%lx: broken magic word (corrupted or freed)",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (s)));
      return false;
    }
  const kind k = s->get_kind ();
  if (static_cast<unsigned> (k) >= n_kinds)
    {
      BAD ("node @%lx: kind %u out of range",
           static_cast<unsigned long> (reinterpret_cast<std::uintptr_t> (s)),
           static_cast<unsigned> (k));
      return false;
    }
  if (want != nullptr && k != *want)
    {
      BAD ("%s built as %s", kind_name (k), kind_name (*want));
      return false;
    }
  if (std::strcmp (s->name (), kind_name (k)) != 0)
    {
      BAD ("%s reports the name %s", kind_name (k), s->name ());
      return false;
    }
  const double got = s->area ();
  if (!(got > 0) || !near_eq (got, s->expected ()))
    {
      BAD ("%s area %s, expected %s", kind_name (k), fx (got, 6).s, fx (s->expected (), 6).s);
      return false;
    }
  return true;
}

// ---- The shared list -------------------------------------------------------

static shape_list g_list;
static mutex g_list_mtx { "shapes" };

static void
lock_list (void)
{
  if (g_list_mtx.lock () != result::ok)
    BAD ("list mutex lock");
}

static void
unlock_list (void)
{
  if (g_list_mtx.unlock () != result::ok)
    BAD ("list mutex unlock");
}

static std::size_t
heap_bytes (void)
{
  return memory::get_default_resource ()->allocated_bytes ();
}

// ---- Threads ---------------------------------------------------------------

static char g_names[16][8];

static thread*
start_thread (unsigned slot, const char* prefix, unsigned n, thread::func_t func,
              std::uintptr_t arg, std::uint32_t cpu_mask,
              thread::priority_t prio = thread::priority::normal)
{
  std::snprintf (g_names[slot], sizeof (g_names[slot]), "%s%u", prefix, n);
  thread::attributes attr = thread::initializer;
  attr.th_stack_size_bytes = thread_stack_bytes; // from the free store
  attr.th_cpu_affinity = cpu_mask;
  attr.th_priority = prio;
  auto* th = new thread (g_names[slot], func, reinterpret_cast<thread::func_args_t> (arg), attr);
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

// ---- Phase 1: four builders, one kind each ---------------------------------

static double g_built_area[n_kinds];

static void*
builder_func (void* arg)
{
  const auto k = static_cast<kind> (reinterpret_cast<std::uintptr_t> (arg));
  const unsigned ki = static_cast<unsigned> (k);
  double sum = 0;
  for (unsigned i = 0; i < per_kind; ++i)
    {
      const double a = 1.0 + 0.5 * i + 0.25 * ki;
      const double b = 2.0 + 0.25 * i;
      shape* s = make (k, a, b);
      if (s == nullptr)
        {
          BAD ("%s #%u: out of memory", kind_name (k), i);
          continue;
        }
      const bool ok = check_shape (s, &k);
      const double area = s->area ();
      lock_list ();
      g_list.push (s);
      unlock_list ();
      sum += area;
      say ("  c%u %-8s #%u: a=%s b=%s area=%s %s\n", cpu_slot (), kind_name (k), i, fx (a, 2).s,
           fx (b, 2).s, fx (area, 4).s, ok ? "ok" : "BAD");
    }
  g_built_area[ki] = sum;
  return nullptr;
}

static double
phase_build (void)
{
  say ("\n-- phase 1: 4 builder threads, %u shapes of each kind\n", per_kind);
  thread* th[n_kinds];
  for (unsigned i = 0; i < n_kinds; ++i)
    th[i] = start_thread (i, "build", i, builder_func, i, 1u << (i % OS_NCPU));
  for (auto* t : th)
    join_and_delete (t);

  std::size_t per[n_kinds] = {};
  std::size_t bad_nodes = 0;
  bool found[n_kinds];
  lock_list ();
  const std::size_t n = g_list.count ();
  const double total = g_list.total_area ();
  for (shape* s = g_list.nodes.head; s != nullptr; s = s->next)
    {
      if (check_shape (s, nullptr))
        ++per[static_cast<unsigned> (s->get_kind ())];
      else
        ++bad_nodes;
    }
  for (unsigned i = 0; i < n_kinds; ++i)
    found[i] = g_list.search (kind_name (kinds[i])) != nullptr;
  unlock_list ();

  double want_total = 0;
  for (double a : g_built_area)
    want_total += a;
  say ("  list: %u shapes (%u circles, %u triangles, %u ellipses, %u rects)\n",
       static_cast<unsigned> (n), static_cast<unsigned> (per[0]), static_cast<unsigned> (per[1]),
       static_cast<unsigned> (per[2]), static_cast<unsigned> (per[3]));
  say ("  total area %s (builders %s)\n", fx (total, 4).s, fx (want_total, 4).s);
  if (n != n_kinds * per_kind || bad_nodes != 0)
    BAD ("list holds %u shapes, %u bad", static_cast<unsigned> (n),
         static_cast<unsigned> (bad_nodes));
  for (unsigned i = 0; i < n_kinds; ++i)
    if (per[i] != per_kind)
      BAD ("%u %ss", static_cast<unsigned> (per[i]), kind_name (kinds[i]));
  if (!near_eq (total, want_total))
    BAD ("total area %s != %s", fx (total, 6).s, fx (want_total, 6).s);
  for (unsigned i = 0; i < n_kinds; ++i)
    if (!found[i])
      BAD ("search(%s) found nothing", kind_name (kinds[i]));
  return total;
}

// ---- Phase 1b: concurrent scaling, one kind per thread ----------------------

static double
kind_area (kind k)
{
  double sum = 0;
  for (shape* s = g_list.nodes.head; s != nullptr; s = s->next)
    {
      if (s->get_kind () == k)
        sum += s->area ();
    }
  return sum;
}

static void
scale_kind (kind k, double factor)
{
  for (shape* s = g_list.nodes.head; s != nullptr; s = s->next)
    {
      if (s->get_kind () == k)
        s->scale (factor);
    }
}

static void*
scaler_func (void* arg)
{
  const auto k = static_cast<kind> (reinterpret_cast<std::uintptr_t> (arg));
  lock_list ();
  const double before = kind_area (k);
  scale_kind (k, 2.0);
  const double doubled = kind_area (k);
  unlock_list ();
  this_thread::yield ();
  lock_list ();
  scale_kind (k, 0.5);
  const double restored = kind_area (k);
  // modify(): the first shape of the kind, 3x and back.
  shape* first = g_list.search (kind_name (k));
  const double a0 = first != nullptr ? first->area () : 0;
  const bool m1 = g_list.modify (kind_name (k), 3.0);
  const double a1 = first != nullptr ? first->area () : 0;
  const bool m2 = g_list.modify (kind_name (k), 1.0 / 3.0);
  const double a2 = first != nullptr ? first->area () : 0;
  unlock_list ();

  const bool ok = near_eq (doubled, 4.0 * before) && near_eq (restored, before) && m1 && m2
                  && near_eq (a1, 9.0 * a0) && near_eq (a2, a0);
  if (!ok)
    BAD ("%s: scale x2 %s -> %s -> %s, modify %s -> %s", kind_name (k), fx (before, 4).s,
         fx (doubled, 4).s, fx (restored, 4).s, fx (a0, 4).s, fx (a1, 4).s);
  say ("  c%u %-8s: area %s -> x2 %s -> back %s; modify x3 %s -> %s %s\n", cpu_slot (),
       kind_name (k), fx (before, 4).s, fx (doubled, 4).s, fx (restored, 4).s, fx (a0, 4).s,
       fx (a1, 4).s, ok ? "ok" : "BAD");
  return nullptr;
}

static void
phase_scale (double base_total)
{
  say ("\n-- phase 1b: 4 threads scale their kind at the same time\n");
  thread* th[n_kinds];
  for (unsigned i = 0; i < n_kinds; ++i)
    th[i] = start_thread (i, "scale", i, scaler_func, i, 1u << (i % OS_NCPU));
  for (auto* t : th)
    join_and_delete (t);
  lock_list ();
  const double total = g_list.total_area ();
  unlock_list ();
  if (!near_eq (total, base_total))
    BAD ("total area after scaling %s != %s", fx (total, 6).s, fx (base_total, 6).s);
  say ("  total area after the round trip %s %s\n", fx (total, 4).s,
       near_eq (total, base_total) ? "ok" : "BAD");
}

// ---- Phase 2: generation / destruction stress -------------------------------

struct worker
{
  unsigned id = 0;
  std::uint64_t rng = 0;
  shape* owned[max_owned] = {};
  unsigned n_owned = 0;
  std::uint32_t created = 0;
  std::uint32_t destroyed = 0;
  std::uint32_t scaled = 0;
  std::uint32_t searched = 0;

  std::uint32_t
  next (void)
  {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t> (rng >> 33);
  }

  double
  param (void)
  {
    return 0.5 + static_cast<double> (next () % 1000) / 100.0;
  }

  void
  create (void)
  {
    const kind k = kinds[next () % n_kinds];
    const double a = param ();
    const double b = param ();
    shape* s = make (k, a, b);
    if (s == nullptr)
      {
        BAD ("w%u: out of memory", id);
        return;
      }
    check_shape (s, &k);
    lock_list ();
    g_list.push (s);
    unlock_list ();
    owned[n_owned++] = s;
    ++created;
  }

  void
  destroy (unsigned idx)
  {
    shape* s = owned[idx];
    lock_list ();
    const bool unlinked = g_list.remove (s);
    unlock_list ();
    if (!unlinked)
      BAD ("w%u: own %s missing from the list", id, s->name ());
    // A broken node is already counted; it is dropped (leaked) rather than
    // deleted through a vtable that may be gone.
    if (check_shape (s, nullptr))
      {
        delete s; // the virtual destructor frees it with its own type
        ++destroyed;
      }
    owned[idx] = owned[--n_owned];
  }

  void
  scale (unsigned idx)
  {
    shape* s = owned[idx];
    const double factor = (next () & 1) == 0 ? 2.0 : 0.5;
    lock_list ();
    const double before = s->area ();
    s->scale (factor);
    const double after = s->area ();
    unlock_list ();
    if (!near_eq (after, before * factor * factor))
      BAD ("w%u: %s scaled %s -> %s", id, s->name (), fx (before, 6).s, fx (after, 6).s);
    ++scaled;
  }

  void
  search (void)
  {
    const kind k = kinds[next () % n_kinds];
    lock_list ();
    const shape* found = g_list.search (kind_name (k));
    const bool kind_ok = found != nullptr && found->get_kind () == k;
    unlock_list ();
    // The 40 phase 1 shapes are always there.
    if (!kind_ok)
      BAD ("w%u: search(%s) failed", id, kind_name (k));
    ++searched;
  }
};

static worker g_workers[n_workers];
static std::atomic<bool> g_stop { false };
static volatile std::uint32_t g_scans = 0;
static volatile unsigned g_round = 0;

static void*
worker_func (void* arg)
{
  worker& w = g_workers[reinterpret_cast<std::uintptr_t> (arg)];
  std::uint32_t ops = 0;
  while (!g_stop.load ())
    {
      const unsigned r = w.next () % 100;
      if (r < 45)
        {
          if (w.n_owned < max_owned)
            w.create ();
          else
            w.destroy (w.next () % w.n_owned);
        }
      else if (r < 75)
        {
          if (w.n_owned > 0)
            w.destroy (w.next () % w.n_owned);
        }
      else if (r < 92)
        {
          if (w.n_owned > 0)
            w.scale (w.next () % w.n_owned);
        }
      else
        {
          w.search ();
        }
      if ((++ops % 8) == 0)
        this_thread::yield ();
    }
  while (w.n_owned > 0)
    w.destroy (w.n_owned - 1);
  say ("  round %2u w%u c%u: +%lu -%lu scaled %lu searched %lu\n", g_round, w.id, cpu_slot (),
       static_cast<unsigned long> (w.created), static_cast<unsigned long> (w.destroyed),
       static_cast<unsigned long> (w.scaled), static_cast<unsigned long> (w.searched));
  return nullptr;
}

static void*
scanner_func (void*)
{
  const std::size_t limit = n_kinds * per_kind + n_workers * max_owned;
  std::uint32_t scans = 0;
  while (!g_stop.load ())
    {
      lock_list ();
      std::size_t n = 0;
      for (shape* s = g_list.nodes.head; s != nullptr; s = s->next)
        {
          if (++n > limit)
            {
              BAD ("scan: more than %u nodes, a cycle?", static_cast<unsigned> (limit));
              break;
            }
          if (!check_shape (s, nullptr))
            break;
        }
      unlock_list ();
      ++scans;
      sysclock.sleep_for (1);
    }
  g_scans = scans;
  return nullptr;
}

static void
phase_stress (double base_total)
{
  say ("\n-- phase 2: %u rounds of %u workers + 1 scanner, %u ms each\n", rounds, n_workers,
       round_ms);
  std::uint64_t created = 0;
  std::uint64_t destroyed = 0;
  for (unsigned r = 0; r < rounds; ++r)
    {
      g_round = r;
      const std::size_t heap_before = heap_bytes ();
      g_stop.store (false);
      g_scans = 0;
      thread* th[n_workers];
      for (unsigned i = 0; i < n_workers; ++i)
        {
          g_workers[i] = worker {};
          g_workers[i].id = i;
          g_workers[i].rng = 0x5EED0000ull + r * 977ull + i * 7919ull;
          th[i] = start_thread (i, "w", i, worker_func, i, 1u << (i % OS_NCPU));
        }
      thread* scanner = start_thread (n_workers, "scan", r, scanner_func, 0, 0xFFFFFFFFu);

      sysclock.sleep_for (round_ms);
      g_stop.store (true);
      for (auto* t : th)
        join_and_delete (t);
      join_and_delete (scanner);

      std::uint32_t c = 0;
      std::uint32_t d = 0;
      for (const auto& w : g_workers)
        {
          c += w.created;
          d += w.destroyed;
        }
      created += c;
      destroyed += d;

      lock_list ();
      const std::size_t n = g_list.count ();
      const double total = g_list.total_area ();
      unlock_list ();
      const std::size_t heap_after = heap_bytes ();
      const bool ok = n == n_kinds * per_kind && near_eq (total, base_total) && c == d
                      && heap_after == heap_before;
      if (!ok)
        BAD ("round %u: list %u, area %s, +%lu -%lu, heap %u -> %u", r,
             static_cast<unsigned> (n), fx (total, 6).s, static_cast<unsigned long> (c),
             static_cast<unsigned long> (d), static_cast<unsigned> (heap_before),
             static_cast<unsigned> (heap_after));
      say ("  round %2u: %u threads joined, +%lu -%lu shapes, %lu scans, list %u, heap %u B %s\n",
           r, n_workers + 1, static_cast<unsigned long> (c), static_cast<unsigned long> (d),
           static_cast<unsigned long> (g_scans), static_cast<unsigned> (n),
           static_cast<unsigned> (heap_after), ok ? "ok" : "BAD");
    }
  say ("  stress total: %llu shapes created, %llu destroyed, %u threads created and destroyed\n",
       static_cast<unsigned long long> (created), static_cast<unsigned long long> (destroyed),
       rounds * (n_workers + 1));
  if (created != destroyed)
    BAD ("%llu shapes created, %llu destroyed", static_cast<unsigned long long> (created),
         static_cast<unsigned long long> (destroyed));
}

// Free the phase 1 shapes.
static void
free_all (void)
{
  for (;;)
    {
      lock_list ();
      shape* s = g_list.nodes.head;
      if (s != nullptr)
        g_list.remove (s);
      unlock_list ();
      if (s == nullptr)
        break;
      if (s->alive ())
        delete s;
      else
        BAD ("broken node while freeing");
    }
}

// ---- The SD card -------------------------------------------------------------

sd::SdCard g_card;

[[noreturn]] static void
fatal (const char* msg)
{
  console ("FATAL: %s\n", msg);
  hw_result::fail ();
  for (;;)
    sysclock.sleep_for (1000);
}

static void
sd_prepare (void)
{
#if defined(HW_BUILD)
  console ("Initialising SD card...\n");
  if (!g_card.init ())
    fatal ("SD card init failed");
  fatfshw::bind_card (g_card);
  if (!fatfshw::mount_volume ())
    fatal ("FAT32 mount failed");
  if (!fatfshw::ensure_tests_dir ())
    fatal ("mkdir tests failed");
  fatfshw::remove_file (log_file);
  console ("  FAT32 /tests ready, logging to tests/%s\n", log_file);
#else
  console ("No SD card in this image: the log goes to the UART only.\n");
#endif
}

// The log on the card read back, its size and CRC compared.
static bool
sd_verify (std::uint32_t* on_card)
{
  *on_card = 0;
#if defined(HW_BUILD)
  if (g_sd_errors != 0)
    return false;
  if (!fatfshw::file_size (log_file, *on_card) || *on_card != g_sd_bytes)
    return false;
  auto* buf = new char[g_sd_bytes];
  if (buf == nullptr)
    return false;
  const bool ok = fatfshw::load_file (log_file, buf, static_cast<std::uint32_t> (g_sd_bytes))
                  && crc32_update (0, buf, g_sd_bytes) == g_sd_crc;
  delete[] buf;
  return ok;
#else
  return true;
#endif
}

// ---- Start-up (as every rpi4b test) ----------------------------------------

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
    uart::uart1 << "\n\n+== " PORT_BANNER_SHORT " µOS++ smp-shapes-test : " TEST_NCPU_STR
                   "-core intrusive list + virtual shapes, UART + SD ==+\n\n";
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

  // The card first, on core 0, before the secondaries run.
  sd_prepare ();

  // The logger, on core 0, above the workers.
  thread* logger = start_thread (15, "logger", 0, logger_func, 0, 1u << 0,
                                 thread::priority::above_normal);
  (void)logger;

  smp_install_boot_threads ();
  smp::start_secondary_cores ();
  const int waited = test_wait_secondaries (3000);
  say ("smp-shapes-test: SMP up (%s) after %d ms\n", test_join_summary (), waited);

  const std::size_t heap_start = heap_bytes ();
  say ("heap at start: %u B\n", static_cast<unsigned> (heap_start));

  const double base_total = phase_build ();
  phase_scale (base_total);
  phase_stress (base_total);

  free_all ();
  const std::size_t heap_end = heap_bytes ();
  say ("\nall shapes freed, heap %u B (at start %u B) %s\n", static_cast<unsigned> (heap_end),
       static_cast<unsigned> (heap_start), heap_end == heap_start ? "ok" : "BAD");
  if (heap_end != heap_start)
    BAD ("heap %u B at the end, %u B at the start", static_cast<unsigned> (heap_end),
         static_cast<unsigned> (heap_start));

  // Everything so far to the card, then read it back.
  log_sync (log_op::flush);
  std::uint32_t on_card = 0;
  const bool sd_ok = sd_verify (&on_card);
#if defined(HW_BUILD)
  say ("SD log tests/%s: %u B written, %lu B on the card, %lu write errors, read-back %s\n",
       log_file, static_cast<unsigned> (g_sd_bytes), static_cast<unsigned long> (on_card),
       static_cast<unsigned long> (g_sd_errors), sd_ok ? "ok" : "BAD");
#endif

  const std::uint32_t errors = g_errors.load ();
  const bool pass = errors == 0 && sd_ok;
  if (pass)
    say ("RESULT: PASS (40 shapes built and checked, %u stress rounds, %s)\n", rounds,
#if defined(HW_BUILD)
         "SD log verified"
#else
         "no SD card in this image"
#endif
    );
  else
    say ("RESULT: FAIL (%lu failed checks, SD log %s)\n", static_cast<unsigned long> (errors),
         sd_ok ? "ok" : "bad");
  log_sync (log_op::stop);
  if (pass)
    hw_result::ok ();
  hw_result::fail ();
  for (;;)
    sysclock.sleep_for (10000);
}
