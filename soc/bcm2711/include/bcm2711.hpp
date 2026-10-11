/**
 * @file  bcm2711.hpp
 * @brief BCM2711 (Raspberry Pi 4 B, 4x Cortex-A72) MMIO map — AArch64.
 *
 * The same namespace and accessor names as bcm2837.hpp, so code written for
 * the Pi 3 family reads the same here. What differs from the BCM2837:
 *
 *   Block                    BCM2837 (Pi 3)        BCM2711 (Pi 4 B)
 *   ----------------------   -------------------   --------------------------
 *   legacy peripherals       0x3F000000            0xFE000000 (low-peri mode)
 *   ARM local peripherals    0x40000000            0xFF800000
 *   interrupt controller     per-core mailboxes +  GIC-400 @ 0xFF840000
 *                            local timer routing   (gic400.hpp)
 *   SD card slot             Arasan SDHCI          EMMC2 @ 0xFE340000
 *                            @ 0x3F300000          (the Arasan at 0xFE300000
 *                                                   is wired to the WiFi)
 *   GPIO pulls               GPPUD + GPPUDCLK      GPIO_PUP_PDN_CNTRL_REG0..3
 *   generic timer            19.2 MHz              54 MHz
 *   PL011 clock              48 MHz                48 MHz (firmware default)
 *
 * The addresses are for the default "low peripheral" mode (arm_peri_high=0 in
 * config.txt). All addresses are ARM-physical; the port maps them 1:1.
 */
#pragma once

#include <cstdint>

namespace bcm {

// --- peripheral (legacy) block --------------------------------------------
inline constexpr std::uint32_t PERIPH_BASE = 0xFE000000u;

// PL011 UART0 (GPIO14/15, pins 8/10 on the header)
inline constexpr std::uint32_t UART0_BASE = PERIPH_BASE + 0x201000u;
namespace pl011 {
inline constexpr std::uint32_t DR     = UART0_BASE + 0x00u; // data
inline constexpr std::uint32_t FR     = UART0_BASE + 0x18u; // flag
inline constexpr std::uint32_t IBRD   = UART0_BASE + 0x24u; // integer baud
inline constexpr std::uint32_t FBRD   = UART0_BASE + 0x28u; // fractional baud
inline constexpr std::uint32_t LCRH   = UART0_BASE + 0x2Cu; // line control
inline constexpr std::uint32_t CR     = UART0_BASE + 0x30u; // control
inline constexpr std::uint32_t IMSC   = UART0_BASE + 0x38u; // interrupt mask
inline constexpr std::uint32_t ICR    = UART0_BASE + 0x44u; // interrupt clear
// FR bits
inline constexpr std::uint32_t FR_TXFF = 1u << 5;           // TX FIFO full
inline constexpr std::uint32_t FR_RXFE = 1u << 4;           // RX FIFO empty
inline constexpr std::uint32_t FR_BUSY = 1u << 3;           // UART busy
}

// GPIO (ACT LED on GPIO 42, UART alt-function select)
inline constexpr std::uint32_t GPIO_BASE   = PERIPH_BASE + 0x200000u;
namespace gpio {
inline constexpr std::uint32_t GPFSEL0  = GPIO_BASE + 0x00u; // fn select, 10 pins/reg
inline constexpr std::uint32_t GPSET0   = GPIO_BASE + 0x1Cu;
inline constexpr std::uint32_t GPCLR0   = GPIO_BASE + 0x28u;
inline constexpr std::uint32_t GPLEV0   = GPIO_BASE + 0x34u;
// 2 bits per pin, 16 pins per register: 0 none, 1 pull-up, 2 pull-down.
inline constexpr std::uint32_t PUP_PDN_CNTRL_REG0 = GPIO_BASE + 0xE4u;
}

// 1 MHz free-running system timer (the reference the generic timer is
// calibrated against, as on the BCM2837).
inline constexpr std::uint32_t SYSTIMER_BASE = PERIPH_BASE + 0x003000u;
namespace systimer {
inline constexpr std::uint32_t CLO = SYSTIMER_BASE + 0x04u;
}

// Power management: the watchdog the hardware runner resets the board with
// (PM_WDOG = 1 tick, then PM_RSTC = full reset, both with the 0x5A password).
inline constexpr std::uint32_t PM_BASE = PERIPH_BASE + 0x100000u;
namespace pm {
inline constexpr std::uint32_t RSTC     = PM_BASE + 0x1Cu;
inline constexpr std::uint32_t WDOG     = PM_BASE + 0x24u;
inline constexpr std::uint32_t PASSWORD = 0x5A000000u;
}

// SD host controllers (SDHCI v3.0).
inline constexpr std::uint32_t EMMC_BASE  = PERIPH_BASE + 0x300000u; // Arasan: WiFi SDIO on the Pi 4
inline constexpr std::uint32_t EMMC2_BASE = PERIPH_BASE + 0x340000u; // the SD card slot

// --- ARM local peripherals block ------------------------------------------
// Still present on the BCM2711, but with the GIC enabled (the firmware
// default, enable_gic=1) the generic timers and the cross-core interrupts go
// through the GIC-400 below, not through this block's per-core routing.
inline constexpr std::uint32_t LOCAL_BASE = 0xFF800000u;

// --- GIC-400 (ARM Generic Interrupt Controller v2) --------------------------
inline constexpr std::uint32_t GIC_BASE     = 0xFF840000u;
inline constexpr std::uint32_t GICD_BASE    = GIC_BASE + 0x1000u;  // distributor
inline constexpr std::uint32_t GICC_BASE    = GIC_BASE + 0x2000u;  // CPU interface

// The whole 0xFC000000 .. 0xFFFFFFFF window is Device memory in the MMU.
inline constexpr std::uint64_t DEVICE_START = 0xFC000000ull;
inline constexpr std::uint64_t DEVICE_END   = 0x100000000ull;

// --- tiny register accessors ----------------------------------------------
inline void     mmio_write(std::uint32_t addr, std::uint32_t v) noexcept {
    *reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(addr)) = v;
}
inline std::uint32_t mmio_read(std::uint32_t addr) noexcept {
    return *reinterpret_cast<volatile std::uint32_t*>(static_cast<std::uintptr_t>(addr));
}

inline void dsb() noexcept { __asm__ volatile("dsb sy" ::: "memory"); }
inline void isb() noexcept { __asm__ volatile("isb" ::: "memory"); }
inline void sev() noexcept { __asm__ volatile("sev" ::: "memory"); }

} // namespace bcm
