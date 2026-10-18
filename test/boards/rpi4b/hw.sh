#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — run ONE test on the Raspberry Pi 4 B, over OpenOCD + CMSIS-DAP JTAG.
#
#     test/boards/rpi4b/hw.sh list                 the tests this build has
#     test/boards/rpi4b/hw.sh <app> [run-seconds]  run one
#
# The session is the shared runner (test_smpl/run-hw.sh); everything here is a
# BCM2711-in-AArch64 fact.
#
# PURE OpenOCD. No GDB, and the UART is NOT touched -- keep your own terminal
# on the USB-serial adapter (GPIO14/15, 115200 8N1): on this board the full
# console is there, and only the RESULT/FAIL/FATAL lines also go through
# semihosting (include/uart.hpp says why: ~4.6 s per trap on this probe).
#
# Each test starts from a reset board: run-hw.sh first resets it through the
# BCM2711 PM watchdog (UOS_HW_PRELOAD below, written through core 0 at its
# normal address -- never `mww phys`, which with the MMU and D-cache left on
# by a previous test makes OpenOCD clean the whole L2 over JTAG first), waits
# UOS_HW_RESET_WAIT seconds for the firmware to boot and re-enable JTAG, then
# loads the test. So tests can run back to back, without a power cycle.
#
# Env:  BUILD=<dir>          the CMake build directory (default: test/build-rpi4b)
#       UOS_HW_RESET_WAIT=s  seconds after the watchdog reset (default: 10)
#       UOS_HW_ADAPTER_KHZ   JTAG clock override. With a CPU bit-banging probe
#                            the USB round trip, not the clock, is the limit;
#                            with the PIO probe firmware (JTAGprobe_smp_pio)
#                            10000-15000 loads ~1.2x faster than 4000
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build-rpi4b}"

CFG="$HERE/openocd-cmsis-dap-rpi4b.cfg"

TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"
# The kernel is the sibling micro-os-plus-iii (the smp branch of
# micro-os-plus-iii-smp.git); an older workspace may keep it as
# micro-os-plus-iii-smp, or as a `*.git` working copy of either name.
_siblings="$(cd "$ROOT/.." && pwd)"
SMP_DIR="${UOS_SMP_DIR:-}"
if [[ -z "$SMP_DIR" ]]; then
  for _d in micro-os-plus-iii micro-os-plus-iii.git \
            micro-os-plus-iii-smp micro-os-plus-iii-smp.git; do
    if [[ -d "$_siblings/$_d" ]]; then SMP_DIR="$_siblings/$_d"; break; fi
  done
  SMP_DIR="${SMP_DIR:-$_siblings/micro-os-plus-iii}"
fi

# The facts:
#   * entry 0x80000, where the AArch64 image is linked;
#   * only core 0 is started (set the PC -- a core keeps the exception level
#     it was halted in, EL2 after the armstub, which is what startup.S
#     dispatches on). Cores 1-3 are resumed where the firmware parked them:
#     the armstub8 spin-table at 0xD8, until the kernel releases them
#     (src/smp.cpp). So __smp_spin is not zeroed here;
#   * the OpenOCD config is declarative, so the runner issues `init`;
#   * the board is reset through the PM watchdog before each test;
#   * only core 0 is halted; cores 1-3 remain in the armstub8 spin-table loop
#     until the kernel releases them (smp.cpp), so OpenOCD does not cache stale
#     registers when CTI restarts them. Semihosting is enabled on all 4 cores.
UOS_HW_CFG="$CFG" \
UOS_HW_CFG_INIT=1 \
UOS_HW_NM="${TC:+$TC/}aarch64-none-elf-nm" \
UOS_HW_READELF="${TC:+$TC/}aarch64-none-elf-readelf" \
UOS_HW_ENTRY=0x80000 \
UOS_HW_SPIN_WORDS=0 \
UOS_HW_RESUME=pc-first \
UOS_HW_NCPU=4 \
UOS_HW_CORES=0 \
UOS_HW_SEMIHOST_CORES="0 1 2 3" \
UOS_HW_TARGET_FMT="bcm2711.cpu%d" \
UOS_HW_RESET_WAIT="${UOS_HW_RESET_WAIT:-10}" \
UOS_HW_IGNORE_ERRORS="${UOS_HW_IGNORE_ERRORS:-JTAG-DP STICKY ERROR|Failed to read memory at 0x80[4-7][0-9a-f]+$}" \
UOS_HW_PRELOAD="${UOS_HW_PRELOAD:-catch {mww 0xfe100024 0x5a000001}; catch {mww 0xfe10001c 0x5a000020}}" \
exec "$SMP_DIR/test_smpl/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"
