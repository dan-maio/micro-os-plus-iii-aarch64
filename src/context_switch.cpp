/*
 * context_switch.cpp - AArch64 context-switch support for the µOS++ port.
 *
 * The cooperative and preemptive switch asm lives in startup.S
 * (port_ctx_switchHandler, irq_current) sharing the ctx64 frame layout with
 * os-core.cpp. This file only provides the fatal reporter for a corrupt
 * next-thread stack pointer detected by the switch paths.
 */
#include <cstdint>
#include <uart.hpp>

extern "C" void port_ctx_switch_corrupt(std::uint64_t new_sp, std::uint64_t old_sp)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    uart::uart1 << "\n!!! CONTEXT SWITCH CORRUPT STACK POINTER !!!\n";
    uart::uart1 << "  new_sp=0x"; uart::uart1.print_hex(static_cast<std::uint32_t>(new_sp >> 32), 8);
    uart::uart1.print_hex(static_cast<std::uint32_t>(new_sp), 8);
    uart::uart1 << "  old_sp=0x"; uart::uart1.print_hex(static_cast<std::uint32_t>(old_sp >> 32), 8);
    uart::uart1.print_hex(static_cast<std::uint32_t>(old_sp), 8);
    uart::uart1 << "\n";
    while (1) __asm__ volatile("wfi");
}
