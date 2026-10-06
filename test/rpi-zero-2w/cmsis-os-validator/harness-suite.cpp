// The harness suite "cmsis-os-validator".
//
// Its sources are not here: they live in the harness,
// tests/sources/cmsis-os-validator/, and are named by board_test_libs() in
// test/rpi-zero-2w/tests.cmake. This file exists because the port's builder
// builds one application per directory that has a *.cpp, and this directory
// is where the application's NAME comes from --
// test/rpi-zero-2w/cmsis-os-validator/ -- so that the platform registers
// cmsis-os-validator-qemu and cmsis-os-validator-hwd like every other test.
//
// It also provides the Cortex-A53 / BCM2837 native interrupt controller glue
// for the validator's interrupt tests (NVIC_* emulation via BCM2837 Mailbox 1).

#include <cstdlib>
#include <cstdint>

#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <semihosting.hpp>
#include <bcm_irq.hpp>

typedef int IRQn_Type;

extern "C" uint32_t SystemCoreClock;
uint32_t SystemCoreClock = 1000000000;

extern "C" void
NVIC_EnableIRQ (IRQn_Type)
{
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      // Clear any pending interrupt on Mailbox 1
      bcm::mmio_write (bcm::local::MBOX_RDCLR0 + 0x10u * c + 4u * 1, ~0u);
      // Enable Mailbox 1 IRQ delivery to this core (bit 1 of MBOX_IRQCNTL0)
      uint32_t ctl = bcm::mmio_read (bcm::local::MBOX_IRQCNTL0 + 4u * c);
      bcm::mmio_write (bcm::local::MBOX_IRQCNTL0 + 4u * c, ctl | (1u << 1));
    }
  bcm::dsb ();
}

extern "C" void
NVIC_DisableIRQ (IRQn_Type)
{
  for (unsigned c = 0; c < OS_NCPU; ++c)
    {
      // Disable Mailbox 1 IRQ delivery to this core (bit 1 of MBOX_IRQCNTL0)
      uint32_t ctl = bcm::mmio_read (bcm::local::MBOX_IRQCNTL0 + 4u * c);
      bcm::mmio_write (bcm::local::MBOX_IRQCNTL0 + 4u * c, ctl & ~(1u << 1));
    }
  bcm::dsb ();
}

extern "C" void
NVIC_SetPendingIRQ (IRQn_Type)
{
  unsigned c = bcm_irq::this_core ();
  bcm::mmio_write (bcm::local::MBOX_SET0 + 0x10u * c + 4u * 1, 1u);
  bcm::dsb ();
  bcm::sev ();
}

extern "C" int
__real_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[]);

extern "C" int
__wrap_os_main (int argc, char* argv[])
{
  // CMSIS-RTOS v1 assumes one CPU: a thread of higher priority preempts its
  // creator at once. The kernel's CMSIS wrapper pins every thread it creates
  // to core 0 (os-c-wrapper.cpp, osThreadCreate); the main thread, which runs
  // the validator, is not created there, so pin it too, and yield so that the
  // scheduler moves it to core 0 before the first test. Without it the main
  // thread keeps running on another core in parallel with the threads it has
  // just created (TC_ThreadGetId fails, TC_ThreadPriority hangs).
  os::rtos::this_thread::thread ().cpu_affinity (1u << 0);
  os::rtos::this_thread::yield ();

  int code = __real_os_main (argc, argv);

  // The UART terminal sees the verdict...
  uart::uart1.puts (code == 0 ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");

  // ...and OpenOCD, which is what hw.sh greps, sees it on the semihosting
  // channel, because report_result() writes there and then stops the run with
  // the semihosting SYS_EXIT -- exactly what the port's own hw tests do.
#if defined(SEMIHOST)
  semihosting::report_result (code == 0);
#else
  std::exit (code);
#endif
  return code;
}
