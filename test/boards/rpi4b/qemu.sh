#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this board's tests under QEMU.
#
#     test/boards/rpi4b/qemu.sh            the whole suite
#     test/boards/rpi4b/qemu.sh <app>      one test
#
# The machine is the board fact: `raspi4b` (QEMU >= 9.0), four Cortex-A72,
# 2 GB, a GIC-400 without Security Extensions. The image is handed straight to
# -kernel; QEMU's boot stub enters it at EL2 and holds cores 1-3 in the
# spin-table at 0xD8, the same contract as the Pi 4 firmware's armstub8.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: test/build-rpi4b)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build-rpi4b}"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"
# A workspace may keep the sibling as a `*.git` working copy, or under the
# kernel's own name.
[ -d "$SMP_DIR" ] || SMP_DIR="$SMP_DIR.git"
[ -d "$SMP_DIR" ] || SMP_DIR="$(cd "$ROOT/.." && pwd)/micro-os-plus-iii"

QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-aarch64 2>/dev/null | sort -V | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-aarch64

UOS_TEST_SRC_DIR="${UOS_TEST_SRC_DIR:-$ROOT/test/rpi4b}" \
UOS_QEMU_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-qemu.sh" "$BUILD/test" "$QEMU" -M raspi4b -smp 4
