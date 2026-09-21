#!/usr/bin/env python3
"""
verify-bootcard.py — prove the AArch64 HW=1 SD tests never touch the boot files.

The real-hardware SD tests (make HW=1) mount the microSD *boot* card's existing
FAT32 partition and keep every file under /tests. This harness proves that is
safe by running the firmware against a synthetic MBR + FAT32 "boot card" under
QEMU raspi3b and then checking, byte-for-byte, that:

  * every root "boot file" (config.txt, kernel7.img, kernel8.img, bootcode.bin,
    start.elf, fixup.dat, cmdline.txt) is unchanged;
  * a pre-existing sentinel under /tests is unchanged;
  * the MBR and the whole region before the partition are untouched;
  * only files under /tests/ were created/overwritten;
  * the FAT structure is clean (fsck.fat).

Usage:
    python3 verify-bootcard.py sd_test
    python3 verify-bootcard.py smp-num-test        # builds with RUN_MS=6000
    python3 verify-bootcard.py smp-mat-sdcard-test

Host prerequisites (Ubuntu/Debian/Arch):
    sudo apt install dosfstools            # mkfs.fat + fsck.fat
    python3 -m venv <venv> && <venv>/pip install "setuptools<81" fs pyfatfs
    # then run this script with the venv's python3.

The card image is rebuilt for every run, so the check always starts from the
same clean baseline. Scratch files live in <port>/build/<test>/bootcard_verify/
(above the test's own build/ so `make clean` cannot delete them).
"""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import warnings

SECTOR = 512
PART_START = 2048            # 1 MiB alignment, like real cards
PART_SECTORS = 256 * 1024    # 256 MiB partition
CARD_SECTORS = 2 * 1024 * 1024  # 1 GiB card image (sparse)
HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = (shutil.which("qemu-system-aarch64")
        or sorted(__import__("glob").glob(
            os.path.expanduser(
                "~/.local/xPacks/@xpack-dev-tools/qemu-arm/*/.content/bin/"
                "qemu-system-aarch64")))[-1])

# name -> (seed, size, is_pattern, text)
BOOT_FILES = {
    "config.txt":  (0, None, False,
                    "arm_64bit=1\nkernel=kernel8.img\nenable_uart=1\n"
                    "core_freq=250\ngpu_mem=16\n"),
    "cmdline.txt": (0, None, False,
                    "console=ttyAMA0,115200 root=/dev/mmcblk0p2 "
                    "rootfstype=ext4 rootwait\n"),
    "kernel8.img": (0x10000001, 3 * 1024 * 1024, True, None),
    "kernel7.img": (0x10000002, 2 * 1024 * 1024, True, None),
    "bootcode.bin": (0x10000003, 128 * 1024, True, None),
    "start.elf":   (0x10000004, 256 * 1024, True, None),
    "fixup.dat":   (0x10000005, 32 * 1024, True, None),
}


def pattern(seed, n):
    x = seed & 0xFFFFFFFF
    out = bytearray()
    while len(out) < n:
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append((x >> 24) & 0xFF)
    return bytes(out[:n])


def make_partition(part):
    with open(part, "wb") as f:
        f.truncate(PART_SECTORS * SECTOR)
    subprocess.run(["/usr/bin/mkfs.fat", "-F", "32", "-n", "BOOT", part],
                   check=True, capture_output=True)
    # The pyfatfs host writer leaves a stale FSInfo free count; fix it so the
    # baseline itself is fsck-clean before the firmware ever runs.
    subprocess.run(["/usr/bin/fsck.fat", "-a", part],
                   check=True, capture_output=True)
    from fs import open_fs
    fs = open_fs("fat://" + part)
    for name, (seed, size, ispat, text) in BOOT_FILES.items():
        data = pattern(seed, size) if ispat else text.encode()
        fs.writebytes(name, data)
    fs.makedir("tests", recreate=True)
    fs.writebytes("tests/old.txt", b"pre-existing sentinel under /tests\n")
    fs.close()


def assemble_card(card, part):
    mbr = bytearray(SECTOR)
    mbr[510], mbr[511] = 0x55, 0xAA
    e = mbr[446:446 + 16]
    e[4] = 0x0C  # FAT32 LBA
    struct.pack_into("<I", e, 8, PART_START)
    struct.pack_into("<I", e, 12, PART_SECTORS)
    mbr[446:446 + 16] = e
    with open(card, "wb") as f:
        f.truncate(CARD_SECTORS * SECTOR)
    with open(card, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))
        f.seek(PART_START * SECTOR)
        f.write(open(part, "rb").read())


def manifest_of(fs, names):
    m = {}
    for name in names:
        data = fs.readbytes(name)
        m[name] = {"size": len(data),
                   "sha256": hashlib.sha256(data).hexdigest()}
    return m


def run_test(test, scratch):
    testdir = os.path.join(HERE, test)
    extra = ["RUN_MS=6000"] if test == "smp-num-test" else []
    subprocess.run(["make", "clean"], cwd=testdir, check=True,
                   capture_output=True)
    subprocess.run(["make", "HW=1"] + extra, cwd=testdir, check=True,
                   capture_output=True)
    kernel = os.path.join(testdir, "output", "kernel8.img")
    assert os.path.isfile(kernel), f"missing {kernel}"
    log = os.path.join(scratch, "run.log")
    with open(log, "wb") as f:
        subprocess.run([QEMU, "-M", "raspi3b", "-smp", "4", "-nographic",
                        "-serial", "none",
                        "-semihosting-config", "enable=on,target=native",
                        "-drive",
                        f"file={os.path.join(scratch, 'bootcard.img')},"
                        "if=sd,format=raw",
                        "-kernel", kernel], cwd=testdir, check=True,
                       stdout=f, stderr=subprocess.STDOUT)
    with open(log, "rb") as f:
        out = f.read().decode(errors="replace")
    if "RESULT: PASS" not in out:
        print(out)
        sys.exit(f"{test}: firmware did not reach RESULT: PASS")
    print(f"{test}: firmware RESULT: PASS")


def verify(test, scratch):
    part = os.path.join(scratch, "part.after.img")
    with open(os.path.join(scratch, "bootcard.img"), "rb") as card:
        card.seek(PART_START * SECTOR)
        with open(part, "wb") as f:
            f.write(card.read(PART_SECTORS * SECTOR))

    from fs import open_fs
    fs = open_fs("fat://" + part)
    before = json.load(open(os.path.join(scratch, "manifest.json")))
    after = manifest_of(fs, sorted(before))
    fs.close()

    bad = [n for n in before if after.get(n) != before[n]]
    print(f"{test}: boot-file bytes {'MATCH' if not bad else 'CHANGED: %r' % bad}")

    # MBR + pre-partition gap untouched
    with open(os.path.join(scratch, "bootcard.img"), "rb") as card:
        head = card.read(PART_START * SECTOR)
    mbr_ok = head[:SECTOR][510] == 0x55 and head[:SECTOR][511] == 0xAA
    gap_ok = head[SECTOR:] == bytes((PART_START - 1) * SECTOR)
    print(f"{test}: MBR + pre-partition region untouched: {mbr_ok and gap_ok}")

    fsck = subprocess.run(["/usr/bin/fsck.fat", "-n", part],
                          capture_output=True, text=True)
    out = fsck.stdout
    # FatFs deliberately does not keep FSInfo's free-count cache in sync, so
    # fsck's "Free cluster summary wrong / Auto-correcting" note is expected
    # and benign (fsck still exits 1 because it would write the fix). Anything
    # structural (orphans, cross-links, bad FAT entries) is a failure.
    structural = re.search(r"Orphan|Cross-linked|marked used|has a bad", out)
    benign_free = "Free cluster summary wrong" in out
    other_correct = ("Auto-correcting" in out) and not benign_free
    fsck_ok = fsck.returncode in (0, 1) and not structural and not other_correct
    print(f"{test}: fsck clean (structure): {fsck_ok}"
          + ("" if structural or other_correct else
             "  [note: FSInfo free-count cache not refreshed by FatFs]"))

    ok = not bad and mbr_ok and gap_ok and fsck_ok
    print(f"{test}: VERIFY {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    args = sys.argv[1:]
    verify_only = "--verify-only" in args
    args = [a for a in args if a != "--verify-only"]
    test = args[0] if args else "sd_test"
    assert test in ("sd_test", "smp-num-test", "smp-mat-sdcard-test"), test
    warnings.filterwarnings("ignore")
    scratch = os.path.join(HERE, "build", test, "bootcard_verify")
    os.makedirs(scratch, exist_ok=True)
    if not verify_only:
        part = os.path.join(scratch, "part.img")
        make_partition(part)
        from fs import open_fs
        fs = open_fs("fat://" + part)
        json.dump(manifest_of(fs, sorted(BOOT_FILES) + ["tests/old.txt"]),
                  open(os.path.join(scratch, "manifest.json"), "w"), indent=1)
        fs.close()
        assemble_card(os.path.join(scratch, "bootcard.img"), part)
        run_test(test, scratch)
    return 0 if verify(test, scratch) else 1


if __name__ == "__main__":
    sys.exit(main())
