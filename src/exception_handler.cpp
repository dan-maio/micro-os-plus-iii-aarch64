/**
 * @file exception_handler.cpp
 * @brief AArch64 fatal-exception reporter for the µOS++ RPi Zero 2W port.
 *
 * Called from the startup.S vector stubs for every non-scheduler exception
 * (synchronous, SError, unexpected FIQ, lower-EL). Dumps the syndrome and
 * halts the offending core. The scheduler IRQ path never comes here.
 */
#include <cstdint>
#include <uart.hpp>
#include <exception_handler.hpp>

namespace exception {
void init() { /* VBAR_EL1 already installed in startup.S */ }
}

namespace {

void hex64(std::uint64_t v)
{
    uart::uart1.print_hex(static_cast<std::uint32_t>(v >> 32), 8);
    uart::uart1.print_hex(static_cast<std::uint32_t>(v), 8);
}

const char* ec_name(std::uint32_t ec)
{
    switch (ec) {
        case 0x15: return "SVC (AArch64)";
        case 0x18: return "MSR/MRS/system";
        case 0x20: case 0x21: return "instruction abort";
        case 0x22: return "PC alignment";
        case 0x24: case 0x25: return "data abort";
        case 0x26: return "SP alignment";
        case 0x2C: return "FP exception";
        case 0x3C: return "BRK";
        default:   return "unknown";
    }
}

} // namespace

extern "C" void port_fatal_exception(std::uint64_t type, std::uint64_t esr,
                                     std::uint64_t elr, std::uint64_t far,
                                     std::uint64_t spsr)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    std::uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    unsigned cpu = static_cast<unsigned>(mpidr) & 3u;
    std::uint32_t ec = static_cast<std::uint32_t>(esr >> 26) & 0x3f;

    uart::uart1 << "\n\n!!! FATAL EXCEPTION (core " << static_cast<int>(cpu)
                << ") !!!\n";
    uart::uart1 << "  vector type = 0x"; uart::uart1.print_hex(static_cast<std::uint32_t>(type), 2);
    uart::uart1 << "\n  ESR_EL1  = 0x"; hex64(esr);
    uart::uart1 << "  (EC=0x"; uart::uart1.print_hex(ec, 2);
    uart::uart1 << " "; uart::uart1 << ec_name(ec); uart::uart1 << ")\n";
    uart::uart1 << "  ELR_EL1  = 0x"; hex64(elr); uart::uart1 << "\n";
    uart::uart1 << "  FAR_EL1  = 0x"; hex64(far); uart::uart1 << "\n";
    uart::uart1 << "  SPSR_EL1 = 0x"; hex64(spsr); uart::uart1 << "\n";
    uart::uart1 << "  core halted.\n";

    while (1) __asm__ volatile("wfi");
}
