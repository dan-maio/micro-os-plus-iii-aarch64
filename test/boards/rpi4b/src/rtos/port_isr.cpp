/*
 * port_isr.cpp - BCM2711 (GIC-400) interrupt dispatch for the AArch64 SMP port.
 *
 * The Zero 2 W's dispatcher with the interrupt controller swapped: instead of
 * reading the BCM2837 "Core N IRQ source" bits, the interrupt is acknowledged
 * at the GIC CPU interface (GICC_IAR), dispatched on its INTID, and ended with
 * GICC_EOIR. One interrupt per entry; anything still pending re-enters.
 *
 * The board's half of src/rtos/os-core.cpp: which interrupt fired, how it is
 * acknowledged, and the sanity check on the stack pointer a switch is about
 * to land on. Everything else in the port's scheduler is board-neutral and
 * lives in the shared file.
 */

#include <cstdint>
#include <cmsis-plus/rtos/os.h>
#include <port_ctx.hpp>
#include <timer_arm.hpp>
#include <gic400.hpp>
#include <uart.hpp>
#include <smp.hpp>

extern "C" unsigned port_cpu_id (void);
extern "C" void* port_switch_stacks (void* sp);
extern "C" void os_systick_handler (void);

extern "C" {
extern volatile bool _in_isr[OS_NCPU];
extern volatile uint32_t _port_core_alive[OS_NCPU];
}

// The pending flags live in the scheduler namespace (SMP port contract), so
// this ISR consults the same object the shared reschedule() writes. A
// C-linkage copy would be a second array nobody else touches.
using os::rtos::port::scheduler::_port_ctx_pending;

static void validate_context(uint64_t *new_sp, uint64_t *old_sp)
{
  if (new_sp < (uint64_t*)PORT_RAM_BASE || new_sp >= (uint64_t*)PORT_RAM_END)
    {
      __asm__ volatile("msr daifset, #0xf" ::: "memory");
      uart::uart1 << "\n!!! CORRUPT STACK POINTER !!!\n";
      (void)old_sp;
      while (1) __asm__ volatile("wfi");
    }

  uint64_t spsr = new_sp[SPSR_IDX];
  if ((spsr & 0x1FU) == MODE_EL1H)
    return;

  __asm__ volatile("msr daifset, #0xf" ::: "memory");
  uart::uart1 << "\n!!! CORRUPT CONTEXT FRAME (bad SPSR) !!!\n";
  while (1) __asm__ volatile("wfi");
}

extern "C" [[gnu::weak]] void port_external_irq_handler(uint32_t source)
{
    (void)source;
}

#if defined(UOS_CMSIS_OS_VALIDATOR)
extern "C" void ARM_CMSIS_VALIDATOR_IRQHandler(void);
#endif

#if defined(UOS_CMSIS_OS_VALIDATOR)
// The validator's NVIC IRQ 0 stand-in (SGI 1) is "enabled" only while this
// flag is set; see test/rpi4b/cmsis-os-validator/harness-suite.cpp.
extern "C" volatile bool uos_validator_irq_enabled;
extern "C" volatile bool uos_validator_irq_pending;
#endif

extern "C" uint64_t * port_isr_handler(uint64_t *current_sp)
{
  unsigned cpu = port_cpu_id();
  _in_isr[cpu] = true;

  const uint32_t iar = gic::acknowledge();
  const unsigned intid = gic::intid_of(iar);

  if (intid == gic::INTID_SPURIOUS || intid == gic::INTID_SPURIOUS_OTHER_GROUP)
    {
      // Nothing to end: a spurious read is not acknowledged.
      _in_isr[cpu] = false;
      return current_sp;
    }

  if (intid == gic::INTID_TIMER_PNS)
    {
      uint32_t freq = timer_arm::frequency();
      timer_arm::set_tval(freq / 1000);

      _port_core_alive[cpu] = 1;

      if (cpu == 0)
        {
          os_systick_handler();
        }

      _port_ctx_pending[cpu] = 1;
    }
  else if (intid == gic::INTID_IPI)
    {
      _port_ctx_pending[cpu] = 1;
    }
#if defined(UOS_CMSIS_OS_VALIDATOR)
  else if (intid == 1u)
    {
      if (uos_validator_irq_enabled)
        {
          gic::end_of_interrupt(iar);
          ARM_CMSIS_VALIDATOR_IRQHandler();
          goto ended;
        }
      uos_validator_irq_pending = true;  // re-raised by NVIC_EnableIRQ
    }
#endif
  else
    {
      port_external_irq_handler(intid);
    }

  gic::end_of_interrupt(iar);
#if defined(UOS_CMSIS_OS_VALIDATOR)
ended:
#endif

  uint64_t *ret_sp = current_sp;
  if (_port_ctx_pending[cpu])
    {
      if (!os::rtos::scheduler::locked () && os::rtos::port::scheduler::_smp_klock.owner != cpu)
        {
          _port_ctx_pending[cpu] = 0;
          uint64_t *new_sp = (uint64_t *)port_switch_stacks(current_sp);
          validate_context(new_sp, current_sp);
          ret_sp = new_sp;
        }
    }

  _in_isr[cpu] = false;
  return ret_sp;
}
