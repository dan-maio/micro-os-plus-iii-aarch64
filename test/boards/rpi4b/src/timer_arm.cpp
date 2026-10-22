/**
 * @file timer_arm.cpp
 * @brief ARM Generic Timer (AArch64) initialization for the BCM2711 A72s.
 *
 * The Zero 2 W's file with the BCM2711 system timer address. The generic
 * timer frequency is calibrated against the 1 MHz system timer instead of
 * trusting CNTFRQ_EL0: the Pi 4 B's counter runs at 54 MHz, and a firmware
 * or loader that left CNTFRQ unset (or at the Pi 3's 19.2 MHz) would make
 * every delay wrong by a factor of 2.8.
 */

#include <timer_arm.hpp>

extern "C" unsigned port_cpu_id(void);
#include <bcm2711.hpp>

namespace timer_arm {

namespace {
volatile std::uint32_t g_freq = 0;
volatile std::uint32_t g_period = 0;  // cycles per 1 ms tick
volatile std::uint64_t g_base = 0;    // core 0's tick origin
volatile bool g_base_set = false;

std::uint32_t st_clo() noexcept {
    return bcm::mmio_read(bcm::systimer::CLO);
}

std::uint32_t calibrate_hw_freq() noexcept {
    // Wait for a clean 1 MHz edge so both windows start aligned.
    std::uint32_t a0 = st_clo();
    while (st_clo() == a0) { }
    std::uint64_t t0 = get_count();
    std::uint32_t s0 = st_clo();

    while ((st_clo() - s0) < 50000u) { }   // 50 ms @ 1 MHz

    std::uint64_t t1 = get_count();
    std::uint32_t s1 = st_clo();
    std::uint32_t dur = s1 - s0;           // us
    if (dur == 0u) { dur = 50000u; }
    return static_cast<std::uint32_t>((t1 - t0) * 1000000ull / dur);
}
} // namespace

std::uint32_t frequency() noexcept {
    if (g_freq == 0) {
        return get_freq();
    }
    return g_freq;
}

void init() noexcept {
    if (g_freq != 0u) {
        return;  // calibrated by the first core; keep it
    }
    // CNTFRQ is the counter's nominal frequency, exact when the firmware set
    // it; the calibration against the System Timer (a 50 ms window, off by
    // a few hundred ppm) only guards against a CNTFRQ left unset or wrong.
    const std::uint32_t cal = calibrate_hw_freq();
    const std::uint32_t reg = get_freq();
    std::uint32_t f = cal;
    if (reg != 0u && (cal == 0u || (reg > cal ? reg - cal : cal - reg) < cal / 100u)) {
        f = reg;
    }
    if (f == 0u) {
        f = reg;
    }
    g_freq = f;
}

void start_1ms() noexcept {
    g_period = frequency() / 1000;
    const std::uint64_t now = get_count();
    if (!g_base_set) {  // core 0, first: the hrclock origin
        g_base = now;
        g_base_set = true;
    }
    set_cval(now + g_period);
    set_ctl(0x1);  // ENABLE=1, IMASK=0
}

void rearm_1ms() noexcept {
    std::uint64_t next = get_cval() + g_period;
    // Far behind (a core halted by the debugger, or by a slow semihosting
    // call: the counter keeps running): resynchronise rather than replay
    // thousands of ticks. The ticks skipped on core 0 are kernel time that
    // never happened, so hrclock's origin moves with them; otherwise
    // hrclock.now() would stay that far ahead of the steady count its sleeps
    // compare with (seen on the Lyra, AArch32: 8.9 s).
    const std::uint64_t now = get_count();
    if (now > next + 1000ull * g_period) {
        const std::uint64_t skipped = (now + g_period - next) / g_period;
        next += skipped * g_period;
        if (port_cpu_id() == 0u) {
            g_base += skipped * g_period;
        }
    }
    set_cval(next);
}

std::uint64_t hr_base() noexcept {
    return g_base;
}

} // namespace timer_arm
