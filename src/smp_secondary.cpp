/*
 * smp_secondary.cpp - secondary-core entry glue. The asm coreN_entry sets up
 * this core's banked stacks + MMU, then calls boot_coreN, which records the
 * join stage and hands off to the shared SMP secondary start (GIC-free timer +
 * mailbox-IPI init, fake context, first reschedule).
 */
#include <cstdint>
#include "smp.hpp"
#include <uart.hpp>

extern "C" void port_smp_secondary_start(void);

// Secondary boot trace: only in DEBUG_BOOT builds (default OFF), so the normal
// test output matches the 32b port exactly.
#ifdef DEBUG_BOOT
#define DBG_BOOT(s) uart::uart1 << (s)
#else
#define DBG_BOOT(s) ((void)0)
#endif

extern "C" void boot_core1()
{
    g_core_stage[1] = 3;
    DBG_BOOT("[boot] core1 in boot_core1\n");
    port_smp_secondary_start();
}

extern "C" void boot_core2()
{
    g_core_stage[2] = 3;
    DBG_BOOT("[boot] core2 in boot_core2\n");
    port_smp_secondary_start();
}

extern "C" void boot_core3()
{
    g_core_stage[3] = 3;
    DBG_BOOT("[boot] core3 in boot_core3\n");
    port_smp_secondary_start();
}
