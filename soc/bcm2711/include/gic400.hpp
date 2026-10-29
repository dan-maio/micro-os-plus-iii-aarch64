/**
 * @file  gic400.hpp
 * @brief GIC-400 (GICv2) glue for the BCM2711 — the Pi 4 B's counterpart of
 *        the BCM2837's bcm_irq.hpp.
 *
 * Two interrupt sources drive the SMP scheduler, exactly as on the BCM2837,
 * only through a different controller:
 *
 *   - the per-core ARM generic timer, non-secure physical timer: PPI 30
 *     (the GICv2 INTID of CNTPNSIRQ on every Cortex-A core);
 *   - the cross-core reschedule IPI: SGI OS_SMP_IPI_SGI (0), sent through
 *     GICD_SGIR to one target core.
 *
 * Unlike the BCM2837 there is an acknowledge protocol: GICC_IAR returns the
 * INTID (and, for an SGI, the sending core), and the same value is written to
 * GICC_EOIR when the interrupt is done.
 *
 * Security. Two configurations reach this code and both must work:
 *
 *   real Pi 4 B   the firmware's armstub (EL3) puts every interrupt in
 *                 Group 1 (non-secure) and the kernel runs non-secure. From
 *                 here GICD_IGROUPR reads as zero and ignores writes, and
 *                 bit 0 of GICD_CTLR / GICC_CTLR enables Group 1.
 *   QEMU raspi4b  the GIC has no Security Extensions: IGROUPR is writable,
 *                 and CTLR bit 0 / bit 1 enable Group 0 / Group 1.
 *
 * So every interrupt is first put in Group 1 (a no-op from non-secure), and
 * Group 1 is enabled. When the group bits are visible, Group 0 is enabled as
 * well, which is harmless because nothing is left in it, and GICC_CTLR.AckCtl
 * is set: without it a read of GICC_IAR in that view returns the special
 * INTID 1022 for a pending Group 1 interrupt instead of acknowledging it --
 * an interrupt storm, which is how the first QEMU run failed. An SGI is sent with
 * NSATT=1, which a non-secure write ignores and which makes a write to a GIC
 * without Security Extensions (or from secure state) send the Group 1 SGI.
 */
#pragma once

#include <cstdint>
#include "bcm2711.hpp"

#ifndef OS_SMP_IPI_SGI
#define OS_SMP_IPI_SGI 0
#endif

namespace gic {

// --- distributor ------------------------------------------------------------
inline constexpr std::uint32_t GICD_CTLR        = bcm::GICD_BASE + 0x000u;
inline constexpr std::uint32_t GICD_TYPER       = bcm::GICD_BASE + 0x004u;
inline constexpr std::uint32_t GICD_IGROUPR0    = bcm::GICD_BASE + 0x080u;
inline constexpr std::uint32_t GICD_ISENABLER0  = bcm::GICD_BASE + 0x100u;
inline constexpr std::uint32_t GICD_ICENABLER0  = bcm::GICD_BASE + 0x180u;
inline constexpr std::uint32_t GICD_ICPENDR0    = bcm::GICD_BASE + 0x280u;
inline constexpr std::uint32_t GICD_IPRIORITYR0 = bcm::GICD_BASE + 0x400u;
inline constexpr std::uint32_t GICD_ITARGETSR0  = bcm::GICD_BASE + 0x800u;
inline constexpr std::uint32_t GICD_SGIR        = bcm::GICD_BASE + 0xF00u;

// --- CPU interface ----------------------------------------------------------
inline constexpr std::uint32_t GICC_CTLR = bcm::GICC_BASE + 0x000u;
inline constexpr std::uint32_t GICC_PMR  = bcm::GICC_BASE + 0x004u;
inline constexpr std::uint32_t GICC_BPR  = bcm::GICC_BASE + 0x008u;
inline constexpr std::uint32_t GICC_IAR  = bcm::GICC_BASE + 0x00Cu;
inline constexpr std::uint32_t GICC_EOIR = bcm::GICC_BASE + 0x010u;

// --- interrupt ids ----------------------------------------------------------
inline constexpr unsigned INTID_IPI       = OS_SMP_IPI_SGI; // reschedule SGI
inline constexpr unsigned INTID_TIMER_PNS = 30u;  // CNTPNSIRQ (EL1 physical timer)
inline constexpr unsigned INTID_SPURIOUS  = 1023u;
// "Pending, but of the other group": also nothing acknowledged, nothing to end.
inline constexpr unsigned INTID_SPURIOUS_OTHER_GROUP = 1022u;

// The priority every interrupt this port enables gets. Anything below the
// PMR passes; 0xA0 is also a valid non-secure (shifted) priority.
inline constexpr std::uint32_t PRIORITY = 0xA0u;

// Core index from MPIDR (Aff0: 0..3 on the BCM2711).
inline unsigned this_core() noexcept {
    std::uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<unsigned>(mpidr) & 3u;
}

namespace detail {

inline void set_priority(unsigned intid, std::uint32_t prio) noexcept {
    const std::uint32_t reg = GICD_IPRIORITYR0 + (intid & ~3u);
    const unsigned sh = (intid & 3u) * 8u;
    std::uint32_t v = bcm::mmio_read(reg);
    v = (v & ~(0xFFu << sh)) | ((prio & 0xFFu) << sh);
    bcm::mmio_write(reg, v);
}

// Put the interrupts of one 32-bit IGROUPR bank in Group 1. Non-secure: RAZ/WI.
inline void group1(unsigned bank) noexcept {
    bcm::mmio_write(GICD_IGROUPR0 + 4u * bank, 0xFFFFFFFFu);
}

// True when the IGROUPR write above took effect, i.e. this code can see the
// group bits: a GIC without Security Extensions (QEMU) or secure state.
inline bool groups_visible() noexcept {
    return bcm::mmio_read(GICD_IGROUPR0) != 0u;
}

} // namespace detail

// Distributor: once, on core 0, before any core enables its CPU interface.
inline void init_distributor() noexcept {
    using namespace bcm;
    const unsigned lines = ((mmio_read(GICD_TYPER) & 0x1Fu) + 1u) * 32u;

    // Shared peripheral interrupts: Group 1, disabled, not pending, to core 0.
    for (unsigned n = 32u; n < lines; n += 32u) {
        detail::group1(n / 32u);
        mmio_write(GICD_ICENABLER0 + n / 8u, 0xFFFFFFFFu);
        mmio_write(GICD_ICPENDR0 + n / 8u, 0xFFFFFFFFu);
    }
    for (unsigned n = 32u; n < lines; n += 4u) {
        mmio_write(GICD_ITARGETSR0 + n, 0x01010101u);
    }

    detail::group1(0u);  // core 0's own SGIs/PPIs (banked)
    // Group 1 forwarding; Group 0 too when the GIC lets this code see groups.
    mmio_write(GICD_CTLR, detail::groups_visible() ? 0x3u : 0x1u);
    dsb();
}

// CPU interface + this core's banked SGI/PPI state: on EVERY core, before it
// unmasks IRQs. Enables the timer PPI and the reschedule SGI.
inline void init_this_core() noexcept {
    using namespace bcm;
    detail::group1(0u);  // SGIs 0-15 and PPIs 16-31 are banked per core

    detail::set_priority(INTID_IPI, PRIORITY);
    detail::set_priority(INTID_TIMER_PNS, PRIORITY);
    mmio_write(GICD_ISENABLER0, (1u << INTID_IPI) | (1u << INTID_TIMER_PNS));

    mmio_write(GICC_PMR, 0xFFu);   // let every priority through
    mmio_write(GICC_BPR, 0u);
    // Groups visible: EnableGrp0 | EnableGrp1 | AckCtl. Non-secure view:
    // bit 0 is EnableGrp1 and the others are not ours to set.
    mmio_write(GICC_CTLR, detail::groups_visible() ? 0x7u : 0x1u);
    dsb();
    isb();
}

// A shared peripheral interrupt (INTID 32 and above): its priority set and
// the line enabled. It goes to core 0, as init_distributor() routes them all.
inline void enable_spi(unsigned intid) noexcept {
    using namespace bcm;
    detail::set_priority(intid, PRIORITY);
    mmio_write(GICD_ISENABLER0 + (intid / 32u) * 4u, 1u << (intid % 32u));
    dsb();
}

// Acknowledge the highest-priority pending interrupt; returns the raw IAR
// value (INTID in [9:0], source core of an SGI in [12:10]).
inline std::uint32_t acknowledge() noexcept {
    return bcm::mmio_read(GICC_IAR);
}

inline unsigned intid_of(std::uint32_t iar) noexcept { return iar & 0x3FFu; }

// Signal the end of the interrupt read by acknowledge() (pass the raw value).
inline void end_of_interrupt(std::uint32_t iar) noexcept {
    bcm::mmio_write(GICC_EOIR, iar);
}

// Send SGI `intid` to one core (target list filter 0 = the listed cores).
inline void send_sgi(unsigned intid, unsigned core) noexcept {
    bcm::dsb();
    bcm::mmio_write(GICD_SGIR,
                    ((1u << (core & 7u)) << 16) | (1u << 15) | (intid & 0xFu));
    bcm::dsb();
}

// The scheduler's reschedule IPI.
inline void send_ipi(unsigned core) noexcept { send_sgi(INTID_IPI, core); }

// Enable / disable one interrupt on the calling core (SGI/PPI) or globally (SPI).
inline void enable(unsigned intid, std::uint32_t prio = PRIORITY) noexcept {
    detail::set_priority(intid, prio);
    bcm::mmio_write(GICD_ISENABLER0 + 4u * (intid / 32u), 1u << (intid & 31u));
    bcm::dsb();
}

inline void disable(unsigned intid) noexcept {
    bcm::mmio_write(GICD_ICENABLER0 + 4u * (intid / 32u), 1u << (intid & 31u));
    bcm::dsb();
}

} // namespace gic
