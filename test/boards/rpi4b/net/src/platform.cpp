// platform.cpp - time, delays, cache maintenance and the console.
#include "platform.hpp"

#include <cmsis-plus/rtos/os.h>
#include <uart.hpp>

#include <cstdarg>
#include <cstdio>

namespace rpi4b::platform
{
  namespace
  {
    inline std::uint64_t
    counter () noexcept
    {
      std::uint64_t value;
      asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (value));
      return value;
    }

    inline std::uint64_t
    frequency () noexcept
    {
      std::uint64_t value;
      asm volatile ("mrs %0, cntfrq_el0" : "=r" (value));
      return value != 0 ? value : 54000000u; // the BCM2711 crystal
    }

    std::uint64_t g_origin = 0;
  } // namespace

  std::uint64_t
  time_us () noexcept
  {
    const std::uint64_t now = counter ();
    if (g_origin == 0)
      {
        g_origin = now;
      }
    const std::uint64_t ticks = now - g_origin;
    const std::uint64_t hz = frequency ();
    // Split to keep the multiplication inside 64 bits for any uptime.
    return (ticks / hz) * 1000000u + ((ticks % hz) * 1000000u) / hz;
  }

  std::uint32_t
  millis () noexcept
  {
    return static_cast<std::uint32_t> (time_us () / 1000u);
  }

  void
  delay_us (std::uint32_t us) noexcept
  {
    if (us >= 1000u && os::rtos::scheduler::started ()
        && !os::rtos::interrupts::in_handler_mode ())
      {
        os::rtos::sysclock.sleep_for ((us + 999u) / 1000u);
        return;
      }
    const std::uint64_t until = time_us () + us;
    while (time_us () < until)
      {
        asm volatile ("yield");
      }
  }

  void
  dcache_flush (const void* address, std::size_t length) noexcept
  {
    constexpr std::uintptr_t line = 64; // Cortex-A72
    std::uintptr_t at = reinterpret_cast<std::uintptr_t> (address) & ~(line - 1);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (address) + length;
    for (; at < end; at += line)
      {
        asm volatile ("dc civac, %0" : : "r" (at));
      }
    asm volatile ("dsb sy; isb");
  }

  void
  dcache_invalidate (const void* address, std::size_t length) noexcept
  {
    constexpr std::uintptr_t line = 64;
    std::uintptr_t at = reinterpret_cast<std::uintptr_t> (address) & ~(line - 1);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t> (address) + length;
    for (; at < end; at += line)
      {
        asm volatile ("dc ivac, %0" : : "r" (at));
      }
    asm volatile ("dsb sy; isb");
  }
} // namespace rpi4b::platform

extern "C" void
kprintf (const char* format, ...)
{
  // One writer at a time: the net thread and the SD thread both report.
  static os::rtos::mutex console{ "console" };
  char text[256];
  std::va_list args;
  va_start (args, format);
  int n = std::vsnprintf (text, sizeof (text), format, args);
  va_end (args);
  if (n < 0)
    {
      return;
    }
  const bool lockable = os::rtos::scheduler::started ()
                        && !os::rtos::interrupts::in_handler_mode ();
  if (lockable)
    {
      console.lock ();
    }
  uart::uart1.puts (text);
  if (lockable)
    {
      console.unlock ();
    }
}

extern "C" unsigned int
net_random (void)
{
  // xorshift64*, seeded from the timer: DHCP transaction ids, DNS ids and
  // TCP initial sequence numbers.
  static std::uint64_t state = 0;
  if (state == 0)
    {
      std::uint64_t now;
      asm volatile ("mrs %0, cntpct_el0" : "=r" (now));
      state = now * 0x9e3779b97f4a7c15ull + 0x2545f4914f6cdd1dull;
    }
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return static_cast<unsigned int> ((state * 0x2545f4914f6cdd1dull) >> 32);
}
