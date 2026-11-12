// platform.hpp - time, delays and the console for the Ethernet stack.
//
// Everything the driver and the lwIP glue need from the board, in one place:
// a monotonic clock from the ARM generic timer, delays that sleep through the
// RTOS when they are long enough to matter, and a printf that keeps a line
// whole when two threads print.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rpi4b::platform
{
  /// Microseconds since the first call (ARM generic timer, CNTPCT_EL0).
  std::uint64_t time_us () noexcept;

  /// Milliseconds since the first call.
  std::uint32_t millis () noexcept;

  /// Wait. One millisecond or more sleeps on the system clock, so other
  /// threads run; shorter waits spin, they are hardware timing constraints.
  void delay_us (std::uint32_t us) noexcept;

  /// Clean and invalidate the data cache over [address, address + length):
  /// what a DMA buffer needs before the device reads it and before the CPU
  /// reads what the device wrote.
  void dcache_flush (const void* address, std::size_t length) noexcept;

  /// Invalidate without writing back: for memory the device has written and
  /// the CPU only reads (a receive buffer).
  void dcache_invalidate (const void* address, std::size_t length) noexcept;
} // namespace rpi4b::platform

extern "C"
{
  /// printf on the console UART; one call is one uninterrupted piece of text.
  void kprintf (const char* format, ...) __attribute__ ((format (printf, 1, 2)));
  unsigned int net_random (void);
}
