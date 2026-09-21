#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh — run this board's tests under QEMU.
#
# Every board has this script, with this interface:
#
#     boards/<id>/qemu.sh            the whole suite
#     boards/<id>/qemu.sh <app>      one test
#
# The machine is the board fact. This one is `raspi3b`, handed the image
# directly: QEMU starts its Cortex-A53 cores in AArch64, which is the state
# this port runs in, so there is nothing to drop out of and no boot shim.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: ../../build)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"

QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-aarch64 2>/dev/null | sort -V | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-aarch64

UOS_TEST_SRC_DIR="$HERE/test" \
UOS_QEMU_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-qemu.sh" "$BUILD/test" "$QEMU" -M raspi3b -smp 4
