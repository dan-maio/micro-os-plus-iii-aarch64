/**
 * @file  semihosting.hpp
 * @brief Minimal ARM semihosting console for the Raspberry Pi Zero 2W (AArch64).
 *
 * The AArch64 sibling of ../../32b/include/semihosting.hpp. Two uses:
 *
 *   - QEMU runs (qemu.sh / run.sh build with SEMIHOST=1): the console arrives
 *     on the host through the semihosting channel, and SYS_EXIT ends the run
 *     as soon as the test prints its RESULT — no idling until `timeout` fires.
 *   - Hardware (J-Link + OpenOCD debug-in-RAM) runs, where the board has no
 *     USB-serial attached and OpenOCD ("arm semihosting enable") is the only
 *     console.
 *
 * The trap is the same on both: HLT #0xF000, with the operation in x0 and the
 * parameter block pointer in x1. (The 32b port has to spell the AArch32
 * encoding out as .inst 0xE10F0070; in A64 the mnemonic assembles directly.)
 *
 * SYS_EXIT differs from AArch32: x1 must point to a TWO-field block
 * { reason, exit_status }, not a bare reason code.
 *
 * When the binary is NOT run under a semihosting-enabled debugger/emulator,
 * every HLT halts the core, so a SEMIHOST build is only for qemu.sh / hw runs.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace semihosting
{
  // SYS_WRITEC (0x03): write the single char pointed to by x1.
  inline void
  write_char (char c) noexcept
  {
    volatile char ch = c;
    register std::uint64_t x0 asm ("x0") = 0x03;
    register volatile char* x1 asm ("x1") = &ch;
    asm volatile ("hlt #0xF000" : "+r" (x0) : "r" (x1) : "memory", "cc");
  }

  // SYS_WRITE0 (0x04): write the NUL-terminated string pointed to by x1.
  inline void
  write_str (const char* s) noexcept
  {
    register std::uint64_t x0 asm ("x0") = 0x04;
    register const char* x1 asm ("x1") = s;
    asm volatile ("hlt #0xF000" : "+r" (x0) : "r" (x1) : "memory", "cc");
  }

  // SYS_WRITE (0x05): write len bytes from buf to the console handle (1), so
  // the buffer need not be NUL-terminated. x1 points to { handle, buf, len }.
  inline void
  write_buf (const char* buf, std::size_t len) noexcept
  {
    volatile std::uint64_t block[3] = {
      1, reinterpret_cast<std::uint64_t> (buf), len
    };
    register std::uint64_t x0 asm ("x0") = 0x05;
    register volatile std::uint64_t* x1 asm ("x1") = block;
    asm volatile ("hlt #0xF000" : "+r" (x0) : "r" (x1) : "memory", "cc");
  }

  // SYS_EXIT (0x18): stop the run and report a status to the host. On AArch64
  // x1 points to { reason, exit_status }: reason 0x20026 is
  // ADP_Stopped_ApplicationExit, and QEMU exits the process with exit_status.
  inline void
  exit (std::uint64_t reason, std::uint64_t status) noexcept
  {
    volatile std::uint64_t block[2] = { reason, status };
    register std::uint64_t x0 asm ("x0") = 0x18;
    register volatile std::uint64_t* x1 asm ("x1") = block;
    asm volatile ("hlt #0xF000" : "+r" (x0) : "r" (x1) : "memory", "cc");
  }

  inline void
  exit_success () noexcept
  {
    constexpr std::uint64_t adp_stopped_applicationexit = 0x20026;
    exit (adp_stopped_applicationexit, 0);
  }

  inline void
  exit_failure () noexcept
  {
    constexpr std::uint64_t adp_stopped_applicationexit = 0x20026;
    exit (adp_stopped_applicationexit, 1);
  }

  // The verdict-and-stop used by a hardware test.
  //
  // A QEMU run has a process status: the test returns from os_main(), the
  // strong _Exit() (src/handlers.cpp) reports through SYS_EXIT, and CTest reads
  // the exit code. A hardware run under OpenOCD has no process status -- the
  // probe is a debugger, not a parent -- so the verdict is the "RESULT:
  // PASS|FAIL" line the runners grep, and SYS_EXIT is what STOPS the run
  // instead of leaving the core spinning until the runner times out.
  //
  // Printing the line on the semihosting channel (not only the UART) is what
  // makes it reach the OpenOCD log, and calling exit_*() is what stops the
  // core.
  [[noreturn]] inline void
  report_result (bool pass) noexcept
  {
    if (pass)
      {
        write_str ("\nRESULT: PASS\n");
        exit_success ();
      }
    else
      {
        write_str ("\nRESULT: FAIL\n");
        exit_failure ();
      }
    for (;;)
      {
      }
  }
} // namespace semihosting
