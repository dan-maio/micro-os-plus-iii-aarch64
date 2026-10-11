/*
 * cxa_guard.cpp - exception-free C++ static-initialisation guards.
 *
 * A function-local static (e.g. test::console_mutex()::m) is guarded by
 * __cxa_guard_acquire/release/abort. libstdc++'s guard.o can throw
 * recursive_init_error, and linking it drags in the whole exception
 * runtime and the DWARF unwinder (~25 KB of every image), which the
 * -fno-exceptions build never uses. Over a 0.6 KB/s JTAG link that is
 * ~40 s per hardware test.
 *
 * These definitions keep the statics thread-safe across cores (the first
 * caller initialises, the others spin until it is done) without throwing.
 * Itanium C++ ABI guard layout: byte 0 != 0 once initialised; byte 1 is
 * used here as the initialisation lock. Recursive initialisation of the
 * same static is undefined behaviour and spins forever.
 */
#include <cstdint>

namespace
{
  inline std::uint8_t*
  done_byte (std::uint64_t* g)
  {
    return reinterpret_cast<std::uint8_t*> (g);
  }

  inline std::uint8_t*
  lock_byte (std::uint64_t* g)
  {
    return reinterpret_cast<std::uint8_t*> (g) + 1;
  }
} // namespace

extern "C" int
__cxa_guard_acquire (std::uint64_t* g)
{
  if (__atomic_load_n (done_byte (g), __ATOMIC_ACQUIRE))
    return 0;
  while (__atomic_exchange_n (lock_byte (g), 1, __ATOMIC_ACQUIRE))
    {
      // Another thread or core is initialising; it finishes or aborts.
      if (__atomic_load_n (done_byte (g), __ATOMIC_ACQUIRE))
        return 0;
    }
  if (__atomic_load_n (done_byte (g), __ATOMIC_ACQUIRE))
    {
      __atomic_store_n (lock_byte (g), 0, __ATOMIC_RELEASE);
      return 0;
    }
  return 1; // caller runs the initialiser, then release (or abort)
}

extern "C" void
__cxa_guard_release (std::uint64_t* g)
{
  __atomic_store_n (done_byte (g), 1, __ATOMIC_RELEASE);
  __atomic_store_n (lock_byte (g), 0, __ATOMIC_RELEASE);
}

extern "C" void
__cxa_guard_abort (std::uint64_t* g)
{
  __atomic_store_n (lock_byte (g), 0, __ATOMIC_RELEASE);
}
