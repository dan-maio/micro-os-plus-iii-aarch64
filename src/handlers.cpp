/**
 * @file handlers.cpp
 * @brief C++ static ctor/dtor runners for the AArch64 µOS++ port.
 *
 * The AArch64 exception vectors live in startup.S; synchronous/SError faults
 * are reported by port_fatal_exception() in exception_handler.cpp. This file
 * only provides the .init_array / .fini_array runners the boot path calls.
 */
#include <cstdint>
#include <uart.hpp>

// The aarch64-none-elf newlib (unlike arm-none-eabi's default specs) does not
// bundle these; provide them for the bare-metal link. µOS++ supplies its own
// _sbrk (heap), so we deliberately do NOT define _sbrk here.
extern "C" {

void* __dso_handle = nullptr;

int  _close (int)                 { return -1; }
int  _lseek (int, int, int)       { return -1; }
int  _read  (int, char*, int)     { return 0; }
int  _fstat (int, void*)          { return 0; }
int  _isatty(int)                 { return 1; }
int  _kill  (int, int)            { return -1; }
int  _getpid(void)                { return 1; }

void _exit (int)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) __asm__ volatile("wfi");
}

int _write (int, const char* buf, int len)
{
    for (int i = 0; i < len; ++i)
    {
        if (buf[i] == '\n')
            uart::uart1.putc('\r');
        uart::uart1.putc(buf[i]);
    }
    return len;
}

} // extern "C"

extern "C" [[gnu::weak]] void __libc_init_array()
{
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    for (auto* fn = __init_array_start; fn != __init_array_end; ++fn)
        (*fn)();
}

extern "C" {

extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

void os_run_fini_array(void)
{
    int count = __fini_array_end - __fini_array_start;
    for (int i = count; i > 0; i--)
    {
        __fini_array_start[i - 1]();
    }
}

}
