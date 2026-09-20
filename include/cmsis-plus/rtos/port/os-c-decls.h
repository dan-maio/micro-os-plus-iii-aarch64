/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 *
 * AArch64 (ARMv8-A, 64-bit) variant for the Raspberry Pi Zero 2W (BCM2837).
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t os_port_clock_timestamp_t;
typedef uint32_t os_port_clock_duration_t;
typedef uint64_t os_port_clock_offset_t;

typedef bool os_port_scheduler_state_t;

/* AArch64 IRQ state is a snapshot of DAIF (bits [9:6]; I-bit = 0x80). */
typedef uint64_t os_port_irq_state_t;

/* AArch64: 64-bit registers, 16-byte stack alignment. Stack is counted in
 * 64-bit elements so stack_ptr math stays 8-byte granular. */
typedef uint64_t os_port_thread_stack_element_t;
typedef uint64_t os_port_thread_stack_allocation_element_t;

typedef struct
{
  os_port_thread_stack_element_t* stack_ptr;
} os_port_thread_context_t;

/* SMP scheduler lock (recursive LDAXR/STLXR spinlock) */
typedef struct {
  volatile uint32_t lock;
  volatile uint32_t owner;
  volatile uint32_t depth;
} os_port_smp_klock_t;

/* SMP timer queue leaf lock (non-recursive) */
typedef struct {
  volatile uint32_t lock;
} os_port_smp_tlock_t;

#define SMP_NO_OWNER  0xFFFFFFFFu
/* The build chooses the core count (uos_add_app NCPU ...); four is the
 * BCM2837 default when nothing says otherwise. */
#ifndef OS_NCPU
#define OS_NCPU       4
#endif
/* BCM2837 has no GIC/SGIs; cross-core reschedule uses local mailbox 0. */
#define OS_SMP_IPI_SGI 0

/* Select the SMP variant of the shared kernel. A single-CPU build of this
 * same port leaves it undefined and gets the scalar scheduler. */
#if !defined(OS_USE_SMP_SCHEDULER) && (OS_NCPU > 1)
#define OS_USE_SMP_SCHEDULER 1
#endif

/* 64-bit stack fill magic (element_t is uint64_t on AArch64). */
#define OS_INTEGER_RTOS_STACK_FILL_MAGIC (0xEFBEADDEEFBEADDEULL)

#define OS_HAS_INTERRUPTS_STACK

#endif /* CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_ */
