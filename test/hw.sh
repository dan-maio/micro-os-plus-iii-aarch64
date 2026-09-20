#!/usr/bin/env bash
# hw.sh — run the AArch64 hardware tests on a Raspberry Pi Zero 2W / Pi 3 B.
#
# This is only the ISA half: the session itself is driven by the shared
# runner in the kernel repository (test/run-hw.sh), which every port uses.
# Everything AArch64-specific is here, and it is four facts:
#
#   * aarch64-none-elf binutils read the ELF;
#   * __smp_spin is uint64_t[4], so eight 32-bit words are zeroed;
#   * resume means "set the PC" -- a core keeps the exception level it was
#     halted in, which is exactly what startup.S dispatches on;
#   * the OpenOCD configs here are declarative, so the runner issues `init`.
#
# PURE OpenOCD: no GDB, no reset, and the UART console is NOT touched --
# keep your own terminal on /dev/ttyACM0.
#
# Usage:  test/hw.sh [app|all] [run-seconds]
# Env:    PROBE=jlink (default) | olimex
#         BUILD=<dir>  the CMake build directory (default: build)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PROBE="${PROBE:-jlink}"

case "$PROBE" in
  jlink)  CFG="$ROOT/boards/rpi-zero-2w/openocd-jlink-rpi3.cfg" ;;
  olimex) CFG="$ROOT/boards/rpi-zero-2w/openocd-olimex.cfg" ;;
  *) echo "hw.sh: PROBE must be jlink or olimex, not '$PROBE'" >&2; exit 2 ;;
esac

# gcc 15 xPack binutils, as the builds use; PATH otherwise.
TC="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/15.*/.content/bin 2>/dev/null | sort -V | tail -1)"

SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

UOS_HW_CFG="$CFG" \
UOS_HW_CFG_INIT=1 \
UOS_HW_NM="${TC:+$TC/}aarch64-none-elf-nm" \
UOS_HW_READELF="${TC:+$TC/}aarch64-none-elf-readelf" \
UOS_HW_ENTRY=0x80000 \
UOS_HW_SPIN_WORDS=8 \
UOS_HW_RESUME=pc \
UOS_HW_NCPU=4 \
exec "$SMP_DIR/test/run-hw.sh" "$BUILD/test" "${1:-all}" "${2:-}"
