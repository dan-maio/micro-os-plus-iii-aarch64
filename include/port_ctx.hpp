/*
 * port_ctx.hpp - the saved-context frame the AArch64 port's assembly builds.
 *
 * Shared by src/rtos/os-core.cpp (which creates and switches frames) and by
 * each board's port_isr.cpp (which validates one before switching to it).
 * The layout is a fact about the ISA and about this port's own assembly, not
 * about any board, so there is one copy.
 */
#pragma once

#include <cstdint>

using std::uint64_t;

struct ctx64
{
  uint64_t x[31];   /* x0 .. x30 */
  uint64_t spsr;    /* SPSR_EL1  */
  uint64_t elr;     /* ELR_EL1 (entry PC) */
  uint64_t fpsr;
  uint64_t fpcr;
  uint64_t pad;
  uint64_t q[64];   /* q0 .. q31, two 64-bit halves each */
};
static_assert(sizeof(ctx64) == 800, "ctx64 must match the 800-byte asm frame");

/* Offset of SPSR within the frame, in element_t (uint64_t) units. */
inline constexpr unsigned SPSR_IDX = 248 / sizeof(uint64_t);   /* = 31 */
inline constexpr unsigned MODE_EL1H = 0x5;                     /* SPSR M[4:0] */
