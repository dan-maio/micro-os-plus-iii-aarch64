# Raspberry Pi 4 B Ethernet: the GENET driver, lwIP and the two tests

The wired counterpart of the Mango Pi MQ-Pro Wi-Fi tests of the RISC-V port: the same lwIP
glue, the same thread layout, an Ethernet controller instead of a radio.

| Part | Where | What it is |
|---|---|---|
| GENET driver | `src/genet.hpp`, `src/genet.cpp` | polled driver for the BCM2711's GENET v5 MAC (`0xFD580000`) and the BCM54213PE gigabit PHY (RGMII, MDIO address 1) |
| lwIP glue | `src/net.hpp`, `src/net.cpp` | interface `en0`, DHCP client, ping, TCP echo server on port 7 |
| Bring-up and SD saver | `src/eth_app.hpp`, `src/eth_app.cpp` | the scripted bring-up both tests share, and the thread that owns the SD card |
| Board services | `src/platform.hpp`, `src/platform.cpp` | time from the ARM generic timer, delays, cache maintenance, `kprintf` |
| lwIP configuration | `src/lwipopts.h`, `src/arch/cc.h` | raw API, no OS layer (`NO_SYS=1`), IPv4, ARP, DHCP, DNS, ICMP, UDP, TCP |
| Start-up | `src/eth_startup.hpp` | the µOS++ start-up every rpi4b test carries, included once per test |
| lwIP 2.2.1 | `../../../third_party/lwip/` | unmodified sources (`core`, `core/ipv4`, `netif`), BSD licence in `COPYING` |
| Tests | `../../../rpi4b/eth-net/`, `../../../rpi4b/eth-tcp-server/` | see below |

Nothing here is compiled into the other tests: `test/rpi4b/tests.cmake` builds these sources
into the INTERFACE library `aarch64-rpi4b-eth`, which only the two Ethernet tests link.

## The driver

The register layout and the bring-up order follow U-Boot's `drivers/net/bcmgenet.c`, the
smallest known-good description of this controller.

- **One receive ring and one transmit ring** (the default queue, 16), 256 descriptors each. The
  descriptors are registers of the controller; only the packet buffers are in RAM.
- **Polled, no interrupts.** `genet::poll()` hands received frames to a handler and checks the
  PHY link four times a second. All controller interrupts are masked.
- **Cache maintenance.** The MMU and the data cache are on. A receive buffer is invalidated
  before it is read and again before it goes back to the controller; the transmit buffer is
  cleaned before the controller is told about it.
- **MAC address** from the firmware (mailbox property tag `0x00010003`), else what the
  controller already holds, else a locally administered address.
- **PHY.** Reset, the two Broadcom shadow registers for the board's `rgmii-rxid` wiring (receive
  clock delayed by the PHY, transmit clock not), auto-negotiation of 10/100/1000. The MAC's
  speed and the RGMII block are set when the link comes up.
- **Transmit** copies the frame into one buffer and waits until the controller has taken it.
  That is enough for a test (about 200 Mbit/s of echoed TCP was measured) and keeps the driver
  free of a transmit queue.
- **Loopback self-test.** `genet::loopback_test()` sends one frame to the board's own address
  and expects it back, looped in the MAC or in the PHY (across RGMII both ways). The bring-up
  runs both when no link comes up, so a controller fault and a cable fault look different.

Not implemented: interrupts, checksum offload, multicast filtering, jumbo frames, EEE.

## The threads

| Thread | Priority | Stack | Role |
|---|---|---|---|
| `net` | above normal | 32 KiB | owns the controller and lwIP; runs the bring-up, the pings and the server; polls every 2 ms |
| `sd` | normal | 64 KiB | owns the SD card and FatFs; mounts the boot card's FAT volume, saves a file when asked |
| `os_main` | normal | 32 KiB | starts both, waits for the verdict, prints `RESULT:` |

- **One thread owns the network.** The driver is polled and lwIP runs without an OS layer:
  neither has any locking. Everything that touches them runs on the `net` thread, including
  the helpers that block (`net::ping` pumps the stack while it waits).
- **The SD card has its own thread.** A FAT write blocks for milliseconds to seconds. The `net`
  thread posts a request and keeps polling until it completes, so the link is serviced while
  the card is busy. Files go to the `tests` folder of the boot card, with 8.3 names; the card
  is never formatted.
- The tests use the single-core start-up of `sd_test`: the kernel is built for four CPUs, but
  cores 1 to 3 stay parked.

## The tests

### `eth-net`

Controller and PHY, link, address, ping, and a log on the SD card.

PASS when the controller and the PHY answer, the link comes up, the interface gets an address,
the gateway answers at least one of four ICMP echoes, and the run's log is written to
`tests/ETHNET.LOG` and read back unchanged.

### `eth-tcp-server`

The same bring-up, then the RFC 862 echo server on port 7. Every byte a client sends comes back
and is kept in RAM (up to 8 MiB). When the client closes, the stream is written to
`tests/ETHRX.BIN` and read back, and the board prints its size and CRC-32. A log goes to
`tests/ETHTCP.LOG`.

PASS when a client sent data and closed, and the stream was saved and verified. A connection
that carries nothing (a port scan) is ignored. Without a client within `ETH_TCP_WAIT_S` seconds
the test fails.

The client is `eth-tcp-server/eth-tcp-host.py` (Python standard library only):

```sh
eth-tcp-host.py 192.168.1.191 --size 1048576     # the address the board printed
eth-tcp-host.py --serial /dev/ttyACM1            # read the address from the console
```

It sends a pattern, checks the echo, and prints the size and CRC-32 it sent. With `--serial` it
also reads the board's CRC and verdict from the console and compares them; use that mode only
when no terminal is open on the port.

### Options

Given to CMake at configure time (`-D NAME=value`); nothing network-specific is in the
repository.

| Option | Default | Meaning |
|---|---|---|
| `ETH_TEST_STATIC_IP` | empty (DHCP) | `a.b.c.d/len`: use this address instead of DHCP |
| `ETH_TEST_GATEWAY` | from DHCP | the gateway `eth-net` pings; required with a static address |
| `ETH_TEST_PEER` | empty | a second host for `eth-net` to ping |
| `ETH_TCP_WAIT_S` | `60` | seconds `eth-tcp-server` waits for a client |

### What a failure says

The `RESULT: FAIL (...)` line reaches the runner through semihosting, so it carries the reason
even when nobody reads the UART:

| Text | Meaning |
|---|---|
| `no GENET controller or PHY (GENET v…, PHY id …)` | the version register or the MDIO bus did not answer as expected |
| `no Ethernet link; 1000: BMSR … LPA … ST1000 … AUX …; 10/100: …` | no link after 7 s with gigabit offered and 8 s more with 10/100 only; the PHY's status registers at both points |
| `no DHCP lease; link … Mbit/s, rx … frames, tx … frames` | the link is up but no address arrived in 20 s |
| `the gateway did not answer` | address obtained, no ICMP echo reply |
| `no TCP client connected` | `eth-tcp-server` waited and nobody sent data |

`LPA 0000` with `AUX 3000` means the PHY sees a partner's negotiation pulses but the
negotiation does not complete: look at the cable and the switch port first.

### QEMU

This port builds a `-qemu` image of every test. For these two it prints `RESULT: SKIP` and
passes; the emulated machine is not given a network.

## Running

From the kernel's harness, as for every rpi4b test (see the kernel repository's
`docs/tests/RPI4B-TESTS-HOWTO.md`, §5a):

```sh
cd "$W/micro-os-plus-iii/tests"
C=aarch64-rpi4b-cmake-gcc-debug
ctest --test-dir build/$C -V -R '^aarch64-rpi4b-eth-net-hwd$'
ctest --test-dir build/$C -V -R '^aarch64-rpi4b-eth-tcp-server-hwd$'
```

## Verified

2026-11-12, Raspberry Pi 4 B on a home router with DHCP, debug configuration, CMSIS-DAP JTAG
probe:

| Case | Result |
|---|---|
| `eth-net` | PASS: GENET v5, PHY id `600d84a2`, firmware MAC address, DHCP lease, gateway ping, `tests/ETHNET.LOG` verified (12 s) |
| `eth-tcp-server` | PASS: 1 MiB from `eth-tcp-host.py`, echo identical, about 200 Mbit/s each way, `tests/ETHRX.BIN` verified (14 s) |
| empty connection before the client | ignored, the test then passed with the real client |
| loopback self-test, link down | MAC ok, PHY ok |
| `-qemu` images | `RESULT: SKIP`, passed |

Not verified: the release configuration, a static address, the 10/100 retry on a link that
needs it, a stream longer than the 8 MiB capture buffer, and the client's `--serial` mode.
