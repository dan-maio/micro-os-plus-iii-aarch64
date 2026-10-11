/**
 * @file  led.hpp
 * @brief User LED helper for the Raspberry Pi 4 B (AArch64).
 *
 * The onboard green ACT LED is a plain SoC GPIO on the Pi 4 B, GPIO 42, and
 * it is active HIGH (the Zero 2 W's is GPIO 29; the Pi 3 B's is behind the
 * VideoCore mailbox). LED_PIN comes from board.cmake. Under QEMU there is no
 * LED -- the register writes are harmless.
 */
#pragma once

#include <cstdint>
#include "bcm2711.hpp"

#ifndef LED_PIN
#define LED_PIN 42
#endif

namespace led {

inline void init() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    const std::uint32_t reg = gpio::GPFSEL0 + (pin / 10) * 4u;
    const unsigned shift = (pin % 10) * 3u;
    std::uint32_t v = mmio_read(reg);
    v &= ~(0x7u << shift);
    v |=  (0x1u << shift);           // 0b001 = output
    mmio_write(reg, v);
}

inline void on() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    mmio_write(gpio::GPSET0 + (pin / 32) * 4u, 1u << (pin % 32));
}

inline void off() noexcept {
    using namespace bcm;
    constexpr unsigned pin = LED_PIN;
    mmio_write(gpio::GPCLR0 + (pin / 32) * 4u, 1u << (pin % 32));
}

inline void set(bool onoff) noexcept { onoff ? on() : off(); }

} // namespace led
