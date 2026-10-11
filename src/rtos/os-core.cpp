/*
 * os-core.cpp - the port's half of the µOS++ III SMP scheduler, AArch64.
 *
 * Shared by every board this port builds for. The interrupt-controller half
 * -- which interrupt fired, how it is acknowledged -- lives in
 * boards/<board>/src/rtos/port_isr.cpp.
 *
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * AArch64 (ARMv8-A, 64-bit, EL1) SMP scheduler port for the BCM2837.
 */

#include <string.h>
#include <cassert>
#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/os-c-decls.h>
#include <port_ctx.hpp>
#include <timer_arm.hpp>
#include <uart.hpp>

#if defined(SEMIHOST)
#include <semihosting.hpp>
#endif

extern "C" void port_sys_init(void);
extern "C" void port_ctx_switchHandler(void);
extern "C" unsigned port_cpu_id(void);

// Secondary bring-up trace: only in DEBUG_BOOT builds (default OFF), so the
// normal test output matches the 32b port exactly.
#ifdef DEBUG_BOOT
#define DBG_BOOT(s) uart::uart1 << (s)
#else
#define DBG_BOOT(s) ((void)0)
#endif

extern os::rtos::thread* os_idle_thread;

// `_port_ctx_pending` is owned by the scheduler namespace (declared in
// cmsis-plus/rtos/port/os-decls.h and defined by the board's port_sys.cpp), so
// it is deliberately NOT redefined here with C linkage: reschedule() and the
// board ISR must share one array.
extern "C" {
volatile bool _in_isr[OS_NCPU] = {false, false, false, false};

/* Per-core "alive" flag: set on this core's first 1ms timer tick. */
volatile uint32_t _port_core_alive[OS_NCPU] = {0, 0, 0, 0};

/* SMP deferred publish slots (per CPU). 64-bit on AArch64: they carry a
 * pointer to old_thread->stack_ptr and the outgoing frame SP. The asm restore
 * path (startup.S SMP_PUBLISH) performs the store only after this CPU's SP has
 * left the outgoing thread's stack. */
volatile uint64_t _smp_pub_addr[OS_NCPU] = {0, 0, 0, 0};
volatile uint64_t _smp_pub_val[OS_NCPU]  = {0, 0, 0, 0};
}

static_assert(sizeof(os_thread_t) == sizeof(os::rtos::thread),
              "CRITICAL ERROR: Size mismatch between C os_thread_t and C++ rtos::thread!");

/* AArch64 context frame (800 bytes) — matches SAVE_FRAME/RESTORE_FRAME in
 * startup.S. Offsets: x[]=0, spsr=248, elr=256, fpsr=264, fpcr=272, q[]=288. */
namespace os
{
  namespace rtos
  {
    namespace scheduler
    {
      extern thread* os_idle_thread_core[OS_NCPU];
    }
  }
}

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace scheduler
      {
        volatile state_t lock_state[OS_NCPU];

        result_t
        initialize (void)
        {
          __asm__ volatile("msr daifset, #2" ::: "memory");
          return result::ok;
        }

        void
        start (void)
        {
          __asm__ volatile("msr daifset, #2" ::: "memory");

          port_sys_init();

          static os_thread_t fake_thread;
          memset (&fake_thread, 0, sizeof (os_thread_t));
          fake_thread.name = "fake_thread";
          rtos::thread* pth = reinterpret_cast<rtos::thread*> (&fake_thread);
          unsigned cpu0 = port_cpu_id();
          rtos::scheduler::current_thread_[cpu0] = pth;

          for (unsigned c = 0; c < OS_NCPU; ++c)
            lock_state[c] = state::init;

          os::rtos::scheduler::os_idle_thread_core[0] = ::os_idle_thread;

          scheduler::reschedule ();

          __asm__ volatile("msr daifclr, #2" ::: "memory");
          for (;;) {
            __asm__ volatile("wfi");
          }
        }

        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          if (state == state::locked)
            {
              __asm__ volatile("msr daifset, #2" ::: "memory");
              unsigned cpu = port_cpu_id();
              state_t tmp = lock_state[cpu];
              if (tmp != state::locked)
                {
                  if (_smp_klock.owner != cpu)
                    {
                      _smp_klock_raw_acquire();
                      _smp_klock.owner = cpu;
                    }
                  _smp_klock.depth = _smp_klock.depth + 1;
                  lock_state[cpu] = state::locked;
                }
              return tmp;
            }
          else
            {
              unsigned cpu = port_cpu_id();
              state_t tmp = lock_state[cpu];
              if (tmp != state::unlocked)
                {
                  lock_state[cpu] = state::unlocked;
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
                  __asm__ volatile("msr daifclr, #2" ::: "memory");
                }
              return tmp;
            }
        }

        void
        reschedule (void)
        {
          unsigned cpu = port_cpu_id();
          if (rtos::scheduler::locked ()
              || (rtos::interrupts::in_handler_mode ()
                  && !rtos::scheduler::preemptive ()))
            {
              return;
            }


          // If this core still owns the kernel spinlock (resume_one() was
          // invoked from inside an interrupts::critical_section, as
          // message_queue::send/receive do), do NOT context-switch here:
          // switching threads while the lock is held would strand the owner
          // and depth on the outgoing thread's stack and deadlock the next
          // acquirer. Defer to the next reschedule/tick, which runs once the
          // caller's critical section has been released.
          if (_smp_klock.owner == cpu && _smp_klock.depth > 0)
            {
              if (cpu < OS_NCPU)
                {
                  _port_ctx_pending[cpu] = 1;
                }
              return;
            }

          if (rtos::interrupts::in_handler_mode ())
            {
              if (cpu < OS_NCPU) {
                _port_ctx_pending[cpu] = 1;
              }
            }
          else
            {
              port_ctx_switchHandler ();
            }
        }

        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          unsigned cpu = port_cpu_id();
          {
            uint32_t tmp, fail;
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
          _smp_klock.owner = cpu;
          _smp_klock.depth = 1;


          rtos::thread* old_thread = rtos::scheduler::current_thread_[cpu];

          rtos::scheduler::internal_switch_threads ();

          rtos::thread* new_thread = rtos::scheduler::current_thread_[cpu];
          if (new_thread == nullptr) {
            __asm__ volatile("msr daifset, #0xf" ::: "memory");
            uart::uart1 << "\n!!! ERROR: no ready thread and no idle thread on Core " << static_cast<int>(cpu) << " !!!\n";
            while (1) __asm__ volatile("wfi");
          }
          stack::element_t* out_sp;
          _smp_pub_addr[cpu] = 0;
          if (new_thread != old_thread) {
            /* Tripwire: the claimed frame MUST lie in the incoming thread's
             * stack and carry a valid EL1h SPSR. */
            {
              class rtos::thread::stack& stk = new_thread->context_.stack ();
              char* lo = reinterpret_cast<char*> (stk.bottom ());
              char* hi = reinterpret_cast<char*> (stk.top ());
              char* p  = reinterpret_cast<char*> (
                  new_thread->context_.port_.stack_ptr);
              bool bad_bounds = (lo != nullptr && hi > lo
                  && (p < lo || p + sizeof(ctx64) > hi));
              /* SPSR sits at SPSR_IDX in the frame; derive the byte offset
               * from the same constant the asm layout and port_isr.cpp use,
               * so a layout change cannot silently desynchronise the check.
               * (The AArch32 tripwire uses ctx_cpsr_word the same way.) */
              uint64_t spsr = (p != nullptr)
                  ? *reinterpret_cast<uint64_t*> (
                        p + SPSR_IDX * sizeof (std::uint64_t))
                  : 0;
              bool bad_frame = (p != nullptr
                  && ((spsr & 0x1FU) != MODE_EL1H));
              if (bad_bounds || bad_frame)
                {
                  __asm__ volatile("msr daifset, #0xf" ::: "memory");
                  uart::uart1 << "\n!!! SMP CLAIM TRIPWIRE (core "
                              << static_cast<int>(cpu) << ") !!!\n  old=";
                  uart::uart1 << (old_thread->name () ? old_thread->name () : "?");
                  uart::uart1 << " new=";
                  uart::uart1 << (new_thread->name () ? new_thread->name () : "?");
                  uart::uart1 << "\n  claimed sp=0x";
                  uart::uart1.print_hex (
                      static_cast<uint32_t> (reinterpret_cast<uint64_t> (p) >> 32), 8);
                  uart::uart1.print_hex (
                      static_cast<uint32_t> (reinterpret_cast<uint64_t> (p)), 8);
                  uart::uart1 << "\n";
                  while (1) __asm__ volatile("wfi");
                }
            }
            _smp_pub_val[cpu]  = reinterpret_cast<uint64_t>(sp);
            _smp_pub_addr[cpu] = reinterpret_cast<uint64_t>(
                &old_thread->context_.port_.stack_ptr);
            out_sp = new_thread->context_.port_.stack_ptr;
            new_thread->context_.port_.stack_ptr = nullptr;
          } else {
            out_sp = sp;
          }

          /* Release the kernel lock. THE ORDER MATTERS: the recursive-lock
           * bookkeeping (owner/depth) must be cleared BEFORE the lock word,
           * which is exactly what every other release site does (clear
           * owner/depth, then _smp_klock_raw_release()).
           *
           * Storing lock=0 first opens a window of two instructions in which
           * another core spinning in _smp_klock_raw_acquire() wins the lock
           * and installs its own owner/depth (interrupts::critical_section::
           * enter()); the two stores below then wipe them. That core's
           * critical_section::exit() is guarded by
           * (owner == cpu && depth > 0), so it never clears the lock word,
           * and from then on every core spins forever in
           * _smp_klock_raw_acquire() -- a silent, total SMP freeze.
           *
           * Same fix as the 32b port (../../32b/src/rtos/os-core.cpp), where
           * this was the root cause of the smp_test4 hardware deadlock; here
           * it stalled smp_test3 after the first queue message. */
          _smp_klock.depth = 0;
          _smp_klock.owner = SMP_NO_OWNER;
          __asm__ volatile("dmb ish" ::: "memory");
          _smp_klock.lock = 0;
          __asm__ volatile("dsb ish" ::: "memory");

          return out_sp;
        }

      } /* namespace scheduler */

      void
      context::create (void* context, void* func, void* args)
      {
        class rtos::thread::context* th_ctx
            = static_cast<class rtos::thread::context*> (context);

        class rtos::thread::stack& stack = th_ctx->stack ();

        rtos::thread::stack::element_t* p = stack.top ();

        /* Align stack top to 16 bytes (AArch64 SP alignment). */
        p = reinterpret_cast<rtos::thread::stack::element_t*> (
            (reinterpret_cast<uintptr_t> (p)) & (~15ULL));

        p -= (sizeof (ctx64) / sizeof (rtos::thread::stack::element_t));

        p = reinterpret_cast<rtos::thread::stack::element_t*> (
            (reinterpret_cast<uintptr_t> (p)) & (~15ULL));

        ctx64* f = reinterpret_cast<ctx64*> (p);

        for (int i = 0; i < 64; ++i)
          f->q[i] = 0;
        f->fpsr = 0;
        f->fpcr = 0;
        f->pad = 0;

        /* SPSR: EL1h (SP_EL1), all interrupts enabled (DAIF = 0). */
        f->spsr = MODE_EL1H;
        f->elr  = reinterpret_cast<uintptr_t> (func);

        f->x[0] = reinterpret_cast<uintptr_t> (args);
        for (int i = 1; i < 30; ++i)
          f->x[i] = 0xCCCCCCCCCCCCCCCCULL;
        f->x[30] = 0;  /* LR */

        th_ctx->port_.stack_ptr = reinterpret_cast<uint64_t*> (p);
      }

      void
      clock_systick::start (void)
      {
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

extern "C" void* port_switch_stacks(void* sp)
{
  return os::rtos::port::scheduler::switch_stacks(
      static_cast<os::rtos::port::stack::element_t*>(sp));
}

// validate_context(), port_external_irq_handler() and port_isr_handler() are
// the only parts of this file that depend on the interrupt controller, so
// they live per board in boards/<board>/src/rtos/port_isr.cpp. Everything
// above and below is the same on every ARMv8-A board this port runs on.
extern "C" void os_systick_handler (void);

extern "C" void port_smp_secondary_start(void)
{
  DBG_BOOT("[sec] secondary start\n");

  __asm__ volatile("msr daifset, #2" ::: "memory");

  unsigned cpu = port_cpu_id();

  port_sys_init();
  DBG_BOOT("[sec] port_sys_init ok\n");

  static os_thread_t fake_thread_secondary[OS_NCPU];
  memset (&fake_thread_secondary[cpu], 0, sizeof (os_thread_t));
  fake_thread_secondary[cpu].name = "fake_thread_sec";
  os::rtos::thread* pth = reinterpret_cast<os::rtos::thread*> (&fake_thread_secondary[cpu]);
  os::rtos::scheduler::current_thread_[cpu] = pth;

  os::rtos::port::scheduler::reschedule ();
  DBG_BOOT("[sec] reschedule returned\n");

  __asm__ volatile("msr daifclr, #2" ::: "memory");
  for (;;) {
    __asm__ volatile("wfi");
  }
}

namespace os
{
  namespace trace
  {
    ssize_t
    write (const void* buf, std::size_t nbyte)
    {
      // A board whose uart.hpp sets PORT_SEMIHOST_MIRROR to 0 keeps its trace
      // on the UART (the Pi 4 B's hwd image: a semihosting trap costs seconds
      // on its probe). Every other board mirrors, as before.
#if defined(SEMIHOST) && (!defined(PORT_SEMIHOST_MIRROR) || PORT_SEMIHOST_MIRROR)
      semihosting::write_buf (reinterpret_cast<const char*> (buf), nbyte);
#endif
      const char* cbuf = reinterpret_cast<const char*> (buf);
      for (std::size_t i = 0; i < nbyte; ++i)
        {
          if (cbuf[i] == '\n')
            {
              uart::uart1.putc_uart ('\r');
            }
          uart::uart1.putc_uart (cbuf[i]);
        }
      return static_cast<ssize_t> (nbyte);
    }
  }
}
