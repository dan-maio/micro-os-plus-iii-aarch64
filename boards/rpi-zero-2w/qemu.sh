#!/usr/bin/env bash
# qemu.sh - run a built µOS++ RPi-Zero-2W AArch64 test under QEMU raspi3b.
#
# raspi3b is QEMU's real Cortex-A53 (BCM2837) model. Booting a raw AArch64
# kernel8.img runs the four A53s natively (no shim needed): the primary enters
# _start at EL2, drops to EL1; the secondaries are released from the spin-table
# by start_secondary_cores(). Same 0x3F000000 peripheral + 0x40000000 local-
# peripheral layout as the Pi Zero 2W.
#
# SEMIHOST is enabled so the console is mirrored to the semihosting channel
# (serial is disabled to avoid printing everything twice) and each test's
# semihosted SYS_EXIT ends the run at its RESULT — no idling until a timeout.
# The test is (re)built here with SEMIHOST=1.
#
# Usage: ./qemu.sh <test-dir> [extra qemu args...]   e.g. ./qemu.sh smp_test0
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${1:-smp_test0}"; shift || true
( cd "$HERE/$DIR" && make SEMIHOST=1 )
IMG="$HERE/$DIR/output/kernel8.img"
[[ -f "$IMG" ]] || { echo "missing $IMG - build first: (cd $DIR && make)"; exit 1; }

QEMU="$(ls "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/qemu-system-aarch64 2>/dev/null | tail -1)"
[[ -x "$QEMU" ]] || QEMU=qemu-system-aarch64

exec "$QEMU" -M raspi3b -smp 4 -nographic -serial none \
     -semihosting-config enable=on,target=native \
     -kernel "$IMG" "$@"
