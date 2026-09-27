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

struct FaultConsole
{
  const FaultConsole& operator<< (const char* str) const noexcept
  {
    uart::uart1.puts_uart (str);
    return *this;
  }

  const FaultConsole& operator<< (char c) const noexcept
  {
    if (c == '\n')
      {
        uart::uart1.putc_uart ('\r');
      }
    uart::uart1.putc_uart (c);
    return *this;
  }

  const FaultConsole& operator<< (int v) const noexcept
  {
    if (v == 0)
      {
        uart::uart1.putc_uart ('0');
        return *this;
      }
    char buf[16];
    char* p = buf + sizeof (buf) - 1;
    *p = '\0';
    unsigned u = static_cast<unsigned> (v < 0 ? -v : v);
    while (u != 0)
      {
        *--p = static_cast<char> ('0' + (u % 10));
        u /= 10;
      }
    if (v < 0)
      {
        *--p = '-';
      }
    uart::uart1.puts_uart (p);
    return *this;
  }
};

static constexpr FaultConsole fcon{};

void print_hex_nibble(std::uint8_t n)
{
    char c = static_cast<char>(n < 10 ? '0' + n : 'A' + (n - 10));
    uart::uart1.putc_uart(c);
}

void print_hex_u32(std::uint32_t val, int digits)
{
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        print_hex_nibble(static_cast<std::uint8_t>((val >> i) & 0xFu));
    }
}

void hex64(std::uint64_t v)
{
    print_hex_u32(static_cast<std::uint32_t>(v >> 32), 8);
    print_hex_u32(static_cast<std::uint32_t>(v), 8);
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

// Fault Status Code (ESR_EL1.FSC, bits [5:0]) for instruction/data aborts.
const char* fsc_name(std::uint32_t fsc)
{
    switch (fsc & 0x3Fu) {
        case 0x00: return "address size fault, level 0";
        case 0x01: return "address size fault, level 1";
        case 0x02: return "address size fault, level 2";
        case 0x03: return "address size fault, level 3";
        case 0x04: return "translation fault, level 0";
        case 0x05: return "translation fault, level 1";
        case 0x06: return "translation fault, level 2";
        case 0x07: return "translation fault, level 3";
        case 0x09: return "access flag fault, level 1";
        case 0x0A: return "access flag fault, level 2";
        case 0x0B: return "access flag fault, level 3";
        case 0x0D: return "permission fault, level 1";
        case 0x0E: return "permission fault, level 2";
        case 0x0F: return "permission fault, level 3";
        case 0x10: return "synchronous external abort";
        case 0x14: return "synchronous external abort, table walk";
        case 0x18: return "synchronous parity/ECC error";
        case 0x1C: return "synchronous parity/ECC error, table walk";
        case 0x21: return "alignment fault";
        default:   return "unknown fault";
    }
}

bool is_abort_ec(std::uint32_t ec)
{
    return ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25;
}

} // namespace

extern "C" void port_fatal_exception(std::uint64_t type, std::uint64_t esr,
                                     std::uint64_t elr, std::uint64_t far,
                                     std::uint64_t spsr)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    std::uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    unsigned cpu = static_cast<unsigned>(mpidr & 0xFFu);
    std::uint32_t ec = static_cast<std::uint32_t>(esr >> 26) & 0x3f;

    fcon << "\n\n!!! FATAL EXCEPTION (core " << static_cast<int>(cpu)
         << ") !!!\n";
    fcon << "  vector type = 0x"; print_hex_u32(static_cast<std::uint32_t>(type), 2);
    fcon << "\n  ESR_EL1  = 0x"; hex64(esr);
    fcon << "  (EC=0x"; print_hex_u32(ec, 2);
    fcon << " "; fcon << ec_name(ec); fcon << ")\n";
    if (is_abort_ec(ec)) {
        std::uint32_t fsc = static_cast<std::uint32_t>(esr) & 0x3Fu;
        fcon << "  Fault    = "; fcon << fsc_name(fsc);
        fcon << " (FSC=0x"; print_hex_u32(fsc, 2); fcon << ")\n";
    }
    fcon << "  ELR_EL1  = 0x"; hex64(elr); fcon << "\n";
    fcon << "  FAR_EL1  = 0x"; hex64(far); fcon << "\n";
    fcon << "  SPSR_EL1 = 0x"; hex64(spsr); fcon << "\n";
    fcon << "  core halted.\n";

    while (1) __asm__ volatile("wfi");
}
