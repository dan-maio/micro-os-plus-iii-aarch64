// The harness suite "cmsis-os-validator".
//
// Its sources are not here: they live in the harness,
// tests/sources/cmsis-os-validator/, and are named by board_test_libs() in
// test/rpi4b/tests.cmake. This file exists because the port's builder
// builds one application per directory that has a *.cpp, and this directory
// is where the application's NAME comes from --
// test/rpi4b/cmsis-os-validator/ -- so that the platform registers
// cmsis-os-validator-qemu and cmsis-os-validator-hwd like every other test.
//
// It also provides the Cortex-A72 / BCM2711 native interrupt controller glue
// for the validator's interrupt tests: NVIC IRQ 0 is emulated with GIC-400
// SGI 1 (the reschedule IPI is SGI 0).
//
// A GIC-400 SGI cannot be disabled (GICD_ISENABLER0 bits 0-15 are RAO/WI), so
// "enabled" is a flag here: rtos/port_isr.cpp calls the validator's handler
// only while it is set, and an SGI raised while it is clear is remembered and
// re-raised on enable -- an NVIC's pending-while-disabled behaviour.

#include <cstdlib>
#include <cstdint>

#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <semihosting.hpp>
#include <gic400.hpp>

typedef int IRQn_Type;

extern "C" uint32_t SystemCoreClock;
uint32_t SystemCoreClock = 1500000000;

// Shared with rtos/port_isr.cpp (UOS_CMSIS_OS_VALIDATOR builds only).
extern "C" volatile bool uos_validator_irq_enabled;
extern "C" volatile bool uos_validator_irq_pending;
volatile bool uos_validator_irq_enabled = false;
volatile bool uos_validator_irq_pending = false;

extern "C" void
NVIC_EnableIRQ (IRQn_Type)
{
  uos_validator_irq_enabled = true;
  __asm__ volatile("dmb ish" ::: "memory");
  if (uos_validator_irq_pending)
    {
      uos_validator_irq_pending = false;
      gic::send_sgi (1u, gic::this_core ());
    }
}

extern "C" void
NVIC_DisableIRQ (IRQn_Type)
{
  uos_validator_irq_enabled = false;
  __asm__ volatile("dmb ish" ::: "memory");
}

extern "C" void
NVIC_SetPendingIRQ (IRQn_Type)
{
  gic::send_sgi (1u, gic::this_core ());
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
