# usb_big_test — a file from the PC over USB, echoed and stored on the SD card (Raspberry Pi 4 B)

The USB part of the tutorial's capstone (lesson 19) on its own: the
Raspberry Pi 4 B as a USB device, receiving a file from the PC, echoing every
byte back as it arrives, and storing the file on the SD card.

## What it does

- **The device.** The BCM2711's DWC2 OTG core, behind the **USB-C** socket,
  as a vendor-specific bulk gadget, VID:PID `1d6b:0104`
  (`drivers/src/usb_dwc2.cpp`, its BCM2711 build: the core at `USB_BASE`, its
  interrupt GIC SPI INTID 105, handed to `usb_dwc2::isr ()` by the test's
  `port_external_irq_handler ()`). One service thread, on core 0 at `high`,
  owns it.
- **The protocol** — the host tool's `--big`:
  1. host → device: one 512-byte packet, `0x02` and the length (32 bits,
     big-endian);
  2. host → device: the file, streamed;
  3. device → host: every byte echoed back as it arrives;
  4. device → host: `0x02`, the count (32 bits, big-endian), `'O'`, `'K'`.
- **The ring.** The bytes are received straight into the blocks of a ring
  (16 × 32 KiB); each full block goes to the card's owner **without
  waiting**, so the bus is never left waiting for the card (the host gives
  up after a few tens of ms of NAKs).
- **The card's owner**, a writer thread (`above_normal`, core 0): it appends
  the blocks, in order, to `tests/usbfile.bin` on the existing FAT32 boot
  partition (never formatted), and frees each one when written.
- **The verdict.** PASS when the file arrived (within 100 s), every byte was
  received, echoed and stored, and the file read back from the card has the
  size and CRC-32 of what was received.

Under QEMU (`raspi4b` models the DWC2 as a host controller only, and has no
boot card) the test prints `RESULT: SKIP`, as `usb_test` does.

## Running it

The PC's cable goes into the Pi 4's USB-C socket.

```sh
# the board (from micro-os-plus-iii-aarch64, BUILD as for every rpi4b test)
test/boards/rpi4b/hw.sh usb_big_test 240

# the PC, once 1d6b:0104 is on the bus (lsusb -d 1d6b:0104)
make -C test/rpi4b/usb_big_test/host
test/rpi4b/usb_big_test/host/output/usb_test_host --big <file>
```

The host tool (`host/`) is the Luckfox Lyra's `usb_test_host`
(`micro-os-plus-iii-aarch32/test/luckfox-lyra/smp_test6/host/`) with the
VID:PID `1d6b:0104`. It reports `PASSED: all N bytes match` and the ack; the
board's verdict and the transfer's numbers are on its UART.

## Files

- `main.cpp` — the test (the start-up as `sd_test`'s).
- `host/` — the host tool (`usb_test_host.cpp`, `libusb_wrapper.hpp`,
  `Makefile`; `usb_recv_host.cpp` is the Lyra bridge's other end, unused
  here).
- `drivers/src/usb_dwc2.cpp`, `drivers/include/usb_dwc2.hpp` — the driver;
  `soc/bcm2711/include/bcm2711.hpp` (`USB_BASE`, `INTID_USB`),
  `gic400.hpp` (`gic::enable_spi ()`).
- `test/rpi4b/tests.cmake` — the test in `BOARD_TEST_NEED_DEVICES`, and the
  driver linked through `aarch64-rpi4b-usb`.

## Results

PASS on the board (2026-10-29): `RESULT: PASS (the host's file echoed and
stored on the SD card)`; on the PC the device enumerated as `1d6b:0104`
("uOS++ bulk test gadget") and the host tool's echo matched. Under QEMU:
SKIP.
