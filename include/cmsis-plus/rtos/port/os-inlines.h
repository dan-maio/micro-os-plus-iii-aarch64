#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/diag/trace.h>
#include <timer_arm.hpp>

#ifdef __cplusplus

/*
 * AArch64 (ARMv8-A, 64-bit, EL1) port inlines for the Raspberry Pi Zero 2W.
 *
 *   CPU id      : MRS MPIDR_EL1, & 3
 *   IRQ mask    : MSR DAIFSET/DAIFCLR, #2   (I bit)
 *   IRQ state   : MRS DAIF                  (I bit = 0x80)
 *   atomics     : LDAXR/STLXR (32-bit lock words) + DMB ISH
 *   in-handler  : per-core _in_isr[] flag (no banked modes in AArch64)
 */

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace scheduler
      {
        inline void __attribute__ ((always_inline))
        greeting (void)
        {
#if defined(SOC_BCM2711)
          trace::printf ("Scheduler: µOS++ Raspberry Pi 4 B (BCM2711, 4x Cortex-A72 SMP, AArch64/EL1, GIC-400, 1ms Tick, LDAXR lock)\n");
#else
          trace::printf ("Scheduler: µOS++ Raspberry Pi Zero 2W (BCM2837, 4x Cortex-A53 SMP, AArch64/EL1, 1ms Tick, LDAXR lock)\n");
#endif
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        lock (void)
        {
          return locked (state::locked);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        unlock (void)
        {
          return locked (state::unlocked);
        }

        inline unsigned __attribute__ ((always_inline))
        port_cpu_id_inline (void)
        {
          uint64_t mpidr;
          __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
          return static_cast<unsigned>(mpidr & 0xFFu);
        }

        inline bool __attribute__ ((always_inline))
        locked (void)
        {
          uint64_t daif;
          __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(daif) :: "memory");
          bool res = (lock_state[port_cpu_id_inline()] != state::unlocked);
          if (!(daif & (1ULL << 7)))
            __asm__ volatile("msr daifclr, #2" ::: "memory");
          return res;
        }

        inline void __attribute__ ((always_inline))
        wait_for_interrupt (void)
        {
          __asm__ volatile("dsb sy" ::: "memory");
          __asm__ volatile("wfi");
        }

        /* SMP: acquire scheduler lock word (LDAXR/STLXR) */
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_acquire (void)
        {
          uint32_t tmp;
          uint32_t fail;
          __asm__ volatile(
            "1:  ldaxr   %w0, [%2]     \n"
            "    cbnz    %w0, 1b       \n"
            "    mov     %w0, #1       \n"
            "    stlxr   %w1, %w0, [%2]\n"
            "    cbnz    %w1, 1b       \n"
            "    dmb     ish           \n"
            : "=&r"(tmp), "=&r"(fail)
            : "r"(&_smp_klock.lock)
            : "cc", "memory");
        }

        /* SMP: release scheduler lock word */
        inline void __attribute__ ((always_inline))
        _smp_klock_raw_release (void)
        {
          __asm__ volatile("dmb ish" ::: "memory");
          _smp_klock.lock = 0;
          __asm__ volatile("dsb ish" ::: "memory");
        }

        /* SMP: enter kernel critical section (mask IRQs + take scheduler lock) */
        inline void __attribute__ ((always_inline))
        port_set_lock (void)
        {
          __asm__ volatile("msr daifset, #2" ::: "memory");
          const unsigned cpu = port_cpu_id_inline ();
          if (_smp_klock.owner != cpu)
          {
            _smp_klock_raw_acquire();
            _smp_klock.owner = cpu;
          }
          _smp_klock.depth = _smp_klock.depth + 1;
        }

        /* SMP: leave kernel critical section */
        inline void __attribute__ ((always_inline))
        port_put_lock (uint64_t lck)
        {
          const unsigned cpu = port_cpu_id_inline ();
          if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
          {
            uint32_t d = _smp_klock.depth - 1;
            _smp_klock.depth = d;
            if (d == 0)
            {
              _smp_klock.owner = SMP_NO_OWNER;
              _smp_klock_raw_release();
            }
          }
          if (lck & 0x80U)
            __asm__ volatile("msr daifset, #2" ::: "memory");
          else
            __asm__ volatile("msr daifclr, #2" ::: "memory");
        }

        /* SMP: full kernel-lock clear (at task body entry) */
        inline void __attribute__ ((always_inline))
        port_clr_lock (void)
        {
          const unsigned cpu = port_cpu_id_inline ();
          if (_smp_klock.owner == cpu)
          {
            _smp_klock.depth = 0;
            _smp_klock.owner = SMP_NO_OWNER;
            _smp_klock_raw_release();
          }
          __asm__ volatile("msr daifclr, #2" ::: "memory");
        }

        /* SMP: migrate lock depth at context switch */
        inline void __attribute__ ((always_inline))
        port_smp_migrate (unsigned depth)
        {
          if (depth == 0)
          {
            _smp_klock.depth = 0;
            _smp_klock.owner = SMP_NO_OWNER;
            _smp_klock_raw_release();
          }
          else
          {
            _smp_klock.depth = depth;
          }
        }

        /* SMP: get current scheduler lock depth */
        inline unsigned __attribute__ ((always_inline))
        port_smp_depth (void)
        {
          return _smp_klock.depth;
        }

        /* SMP: acquire timer leaf lock */
        inline void __attribute__ ((always_inline))
        port_tmr_lock (void)
        {
          uint32_t tmp;
          uint32_t fail;
          __asm__ volatile(
            "1:  ldaxr   %w0, [%2]     \n"
            "    cbnz    %w0, 1b       \n"
            "    mov     %w0, #1       \n"
            "    stlxr   %w1, %w0, [%2]\n"
            "    cbnz    %w1, 1b       \n"
            "    dmb     ish           \n"
            : "=&r"(tmp), "=&r"(fail)
            : "r"(&_smp_tlock.lock)
            : "cc", "memory");
        }

        /* SMP: release timer leaf lock */
        inline void __attribute__ ((always_inline))
        port_tmr_unlock (void)
        {
          __asm__ volatile("dmb ish" ::: "memory");
          _smp_tlock.lock = 0;
          __asm__ volatile("dsb ish" ::: "memory");
        }
      }

      namespace interrupts
      {
        extern "C" volatile bool _in_isr[OS_NCPU];

        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          const unsigned cpu = scheduler::port_cpu_id_inline ();
          return (cpu < OS_NCPU) && _in_isr[cpu];
        }

        inline bool
        is_priority_valid (void)
        {
          return true;
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        critical_section::enter (void)
        {
          uint64_t daif;
          __asm__ volatile("mrs %0, daif" : "=r"(daif));
          __asm__ volatile("msr daifset, #2" ::: "memory");

          const unsigned cpu = scheduler::port_cpu_id_inline ();
          if (scheduler::_smp_klock.owner != cpu)
          {
            scheduler::_smp_klock_raw_acquire();
            scheduler::_smp_klock.owner = cpu;
          }
          scheduler::_smp_klock.depth = scheduler::_smp_klock.depth + 1;

          return daif;
        }

        inline void __attribute__ ((always_inline))
        critical_section::exit (rtos::interrupts::state_t state)
        {
          const unsigned cpu = scheduler::port_cpu_id_inline ();
          if (scheduler::_smp_klock.owner == cpu && scheduler::_smp_klock.depth > 0)
          {
            uint32_t d = scheduler::_smp_klock.depth - 1;
            scheduler::_smp_klock.depth = d;
            if (d == 0)
            {
              scheduler::_smp_klock.owner = SMP_NO_OWNER;
              scheduler::_smp_klock_raw_release();
            }
          }
          if (state & 0x80U)
            __asm__ volatile("msr daifset, #2" ::: "memory");
          else
            __asm__ volatile("msr daifclr, #2" ::: "memory");
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        uncritical_section::enter (void)
        {
          uint64_t daif;
          __asm__ volatile("mrs %0, daif" : "=r"(daif));
          __asm__ volatile("msr daifclr, #2" ::: "memory");
          return daif;
        }

        inline void __attribute__ ((always_inline))
        uncritical_section::exit (rtos::interrupts::state_t state)
        {
          if (state & 0x80U)
            __asm__ volatile("msr daifset, #2" ::: "memory");
          else
            __asm__ volatile("msr daifclr, #2" ::: "memory");
        }
      }

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
        }
      }

      inline void __attribute__ ((always_inline))
      clock_highres::start (void)
      {
      }

      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return true;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        // From core 0's tick origin, so that it agrees with the tick-driven
        // count the kernel compares hrclock timestamps with.
        return timer_arm::get_count () - timer_arm::hr_base ();
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::input_clock_frequency_hz (void)
      {
        return timer_arm::frequency();
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_per_tick (void)
      {
        return timer_arm::frequency() / 1000;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_since_tick (void)
      {
        uint32_t load_value = cycles_per_tick();
        uint32_t val = timer_arm::get_tval();
        if (val > load_value) {
          return 0;
        }
        return load_value - val;
      }

      /* SMP: 32-bit compare-and-swap (LDAXR/STLXR) */
      inline bool __attribute__ ((always_inline))
      port_cas32 (volatile uint32_t *ptr,
                   uint32_t expected, uint32_t desired)
      {
        uint32_t old;
        uint32_t fail;
        __asm__ volatile(
          "    ldaxr   %w0, [%2]     \n"
          "    cmp     %w0, %w3      \n"
          "    b.ne    1f            \n"
          "    stlxr   %w1, %w4, [%2]\n"
          "    cbnz    %w1, 2f       \n"
          "    dmb     ish           \n"
          "    b       3f            \n"
          "1:  clrex                 \n"
          "2:  mov     %w1, #1       \n"
          "    b       4f            \n"
          "3:  mov     %w1, #0       \n"
          "4:                        \n"
          : "=&r"(old), "=&r"(fail)
          : "r"(ptr), "r"(expected), "r"(desired)
          : "cc", "memory");
        return fail == 0 && old == expected;
      }

    }
  }
}

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
