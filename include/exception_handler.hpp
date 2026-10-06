/**
 * @file exception_handler.hpp
 * @brief AArch64 exception handling entry points for the RPi Zero 2W port.
 *
 * The EL1 vector table is installed in startup.S (VBAR_EL1). Synchronous /
 * SError / unexpected exceptions are reported by port_fatal_exception()
 * (exception_handler.cpp). exception::init() is a hook the app calls during
 * hardware init; the vectors are already live by then, so it is a no-op kept
 * for source compatibility with the AArch32 port.
 */
#pragma once

#include <cstdint>

namespace exception {

void init();

} // namespace exception

extern "C" void port_fatal_exception(std::uint64_t type, std::uint64_t esr,
                                     std::uint64_t elr, std::uint64_t far,
                                     std::uint64_t spsr);
