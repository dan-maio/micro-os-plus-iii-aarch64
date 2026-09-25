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

#if defined(SEMIHOST)
#include <semihosting.hpp>
#endif

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

// Strong exit: terminate through the port's AArch64 semihosting SYS_EXIT
// (HLT #0xF000, x1 -> { reason, status }), so std::exit() -- i.e. returning
// from os_main() -- ends the run with a real exit code (0 on success under
// QEMU) instead of idling. The kernel's _Exit() is weak; this replaces it.
static void
port_exit (int code)
{
#if defined(SEMIHOST)
    if (code == 0)
        semihosting::exit_success ();
    else
        semihosting::exit_failure ();
#else
    (void)code;
#endif
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) __asm__ volatile("wfi");
}

void _exit (int code) { port_exit (code); }
void _Exit (int code) { port_exit (code); }

int _write (int, const char* buf, int len)
{
    // The board console is the UART; under SEMIHOST also copy to the
    // debugger's semihosting console, so the suites' printf() output shows up
    // in the run log as well as on the serial terminal. Always in addition to
    // the UART, never instead of it.
#if defined(SEMIHOST)
    semihosting::write_buf(buf, static_cast<std::size_t>(len));
#endif
    for (int i = 0; i < len; ++i)
    {
        if (buf[i] == '\n')
            uart::uart1.putc_uart('\r');
        uart::uart1.putc_uart(buf[i]);
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
