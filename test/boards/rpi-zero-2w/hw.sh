#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh — run ONE test on this board, over OpenOCD.
#
#     test/boards/<id>/hw.sh list                 the tests this build has
#     test/boards/<id>/hw.sh <app> [run-seconds]  run one
#
# The session is the shared runner (test_smpl/run-hw.sh); everything here is a
# BCM2837-in-AArch64 fact.
#
# PURE OpenOCD. No GDB, no reset, and the UART is NOT touched -- keep your own
# terminal on /dev/ttyACM0.
#
# ONE TEST PER POWER CYCLE: a load into RAM over whatever the previous test
# left there, and the Pi has no SRST a script can drive.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: test/build)
#       PROBE=jlink | olimex                      (default: jlink)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"
PROBE="${PROBE:-jlink}"

case "$PROBE" in
  jlink)  CFG="$HERE/openocd-jlink-rpi3.cfg" ;;
  olimex) CFG="$HERE/openocd-olimex.cfg" ;;
  *) echo "hw.sh: PROBE must be jlink or olimex, not '$PROBE'" >&2; exit 2 ;;
esac

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

# The facts that differ from the 32-bit port on the same silicon:
#   * entry 0x80000, where the AArch64 image is linked;
#   * only core 0 is started (set the PC -- a core keeps the exception level
#     it was halted in, which is what startup.S dispatches on). Cores 1-3 are
#     resumed where they were halted: in the firmware's spin-table loop after
#     the watchdog reset, until the kernel releases them (smp.cpp) once it
#     has written their __smp_spin slot. So __smp_spin is not zeroed here.
#     This needs an SD card whose kernel leaves cores 1-3 in that loop
#     (e.g. hw-park/); the previous all-cores flow is kept in hw.sh.bak;
#   * these OpenOCD configs are declarative, so the runner issues `init`.
UOS_HW_CFG="$CFG" \
UOS_HW_CFG_INIT=1 \
UOS_HW_NM="${TC:+$TC/}aarch64-none-elf-nm" \
UOS_HW_READELF="${TC:+$TC/}aarch64-none-elf-readelf" \
UOS_HW_ENTRY=0x80000 \
UOS_HW_SPIN_WORDS=0 \
UOS_HW_RESUME=pc-first \
UOS_HW_NCPU=4 \
UOS_HW_TARGET_FMT="bcm2837.cpu%d" \
UOS_HW_PRELOAD="${UOS_HW_PRELOAD:-mww 0x3f100024 0x5a000001; mww 0x3f10001c 0x5a000020}" \
exec "$SMP_DIR/test_smpl/run-hw.sh" "$BUILD/test" "${1:-list}" "${2:-}"

