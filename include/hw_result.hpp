/**
 * @file  hw_result.hpp
 * @brief Bounded "done" helper for the rpi-zero-2w 64b tests.
 *
 * The AArch64 sibling of ../../32b/include/hw_result.hpp. A test prints its
 * RESULT line and then calls ok()/fail(); on a SEMIHOST build that ends the
 * run through a semihosted SYS_EXIT, so:
 *
 *   - QEMU quits the moment the verdict is known (qemu.sh / run.sh) instead of
 *     idling until the wrapping `timeout` expires;
 *   - an OpenOCD debug-in-RAM run stops with a PASS/FAIL exit status.
 *
 * Without SEMIHOST these compile to nothing and the tests keep their original
 * idle-forever behaviour, so a plain `make` image is unchanged.
 *
 * Pattern in a test:
 *
 *     uart1 << "\nRESULT: " << (ok ? "PASS" : "FAIL") << "\n";
 *     if (ok) hw_result::ok (); else hw_result::fail ();
 *     for (;;) sysclock.sleep_for (1000);   // never reached on SEMIHOST
 */
#pragma once

#if defined(SEMIHOST)
#include "semihosting.hpp"
#endif

namespace hw_result
{
  // Stop the run reporting SUCCESS. On SEMIHOST builds this never returns
  // (SYS_EXIT stops the guest); otherwise it is a no-op.
  inline void
  ok () noexcept
  {
#if defined(SEMIHOST)
    semihosting::exit_success ();
#endif
  }

  // Stop the run reporting FAILURE. Never returns on SEMIHOST builds.
  inline void
  fail () noexcept
  {
#if defined(SEMIHOST)
    semihosting::exit_failure ();
#endif
  }
} // namespace hw_result
