/*
 * usb_big_test — Raspberry Pi 4 B (BCM2711, 4× Cortex-A72): a file from the
 * PC over USB, echoed back and stored on the SD card.
 *
 * The USB part of the tutorial's capstone (Work2's
 * test/rpi4b/tutor/19-capstone/app/usb/ and storage/), on its own:
 *
 *   - the DWC2 core behind the USB-C socket as a vendor-specific bulk gadget,
 *     1d6b:0104 (drivers/src/usb_dwc2.cpp, the BCM2711 build: its interrupt
 *     is GIC SPI INTID 105, handed to usb_dwc2::isr () below), owned by one
 *     service thread on core 0;
 *   - the host tool's --big protocol (host/usb_test_host.cpp):
 *       host -> device: one 512-byte packet: 0x02, the length (32 bits,
 *                       big-endian);
 *       host -> device: the file, streamed;
 *       device -> host: every byte echoed back as it arrives;
 *       device -> host: 0x02, the count (32 bits, big-endian), 'O', 'K';
 *   - the bytes received straight into the blocks of a ring (16 × 32 KiB),
 *     each full block handed to the card's owner without waiting, so the bus
 *     is never left waiting for the card;
 *   - the card's owner, a writer thread: tests/usbfile.bin on the existing
 *     FAT32 boot partition (never formatted);
 *   - at the end the file read back: its size and CRC-32 must be those
 *     received.
 *
 * Run:  test/boards/rpi4b/hw.sh usb_big_test 240, then on the PC (the cable
 * in the Pi 4's USB-C socket), once 1d6b:0104 is on the bus:
 *       host/output/usb_test_host --big <file>
 * The board waits at most 100 s for the transfer. Board only: QEMU's raspi4b
 * has no device-mode DWC2 and no card.
 */
#include <cmsis-plus/rtos/os.h>

#include <uart.hpp>
#include <exception_handler.hpp>
#include <bcm2711.hpp>
#include <sd.hpp>
#include <fatfs_hw.hpp>
#include <usb_dwc2.hpp>
#include <hw_result.hpp>

#include "test-console.hpp"

#include <cstdint>
#include <cstdlib>

using namespace os::rtos;

// ---------------------------------------------------------------------------
// The USB core's interrupt: every interrupt the board does not own comes here
// (test/boards/rpi4b/src/rtos/port_isr.cpp, the weak default replaced).
extern "C" void
port_external_irq_handler (std::uint32_t intid)
{
  if (intid == bcm::INTID_USB)
    usb_dwc2::isr ();
}

namespace
{
  constexpr const char* usb_leaf = "usbfile.bin";
  constexpr unsigned usb_wait_s = 100;

  constexpr thread::priority_t prio_usb = thread::priority::high;
  constexpr thread::priority_t prio_card = thread::priority::above_normal;

  // The System Timer's free-running 1 MHz counter: microseconds, for the
  // transfer's timings.
  std::uint32_t
  now_us (void)
  {
    return *reinterpret_cast<volatile std::uint32_t*> (bcm::PERIPH_BASE + 0x3004u);
  }

  // CRC-32 (zlib's), continued from crc (0 to start).
  std::uint32_t
  crc32 (std::uint32_t crc, const void* data, std::size_t len)
  {
    const auto* p = static_cast<const std::uint8_t*> (data);
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i)
      {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
          crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
      }
    return ~crc;
  }

  thread::attributes
  attributes_for (thread::priority_t prio, void* stack, std::size_t bytes)
  {
    thread::attributes a = thread::initializer;
    a.th_priority = prio;
    a.th_stack_address = stack;
    a.th_stack_size_bytes = bytes;
#if defined(OS_USE_SMP_SCHEDULER)
    a.th_cpu_affinity = 1u << 0; // core 0, with the device and the card
#endif
    return a;
  }

  // ---- The card: one owner, the writer thread ------------------------------
  //
  // The USB thread fills a block and hands it over without waiting; the writer
  // appends the blocks, in order, to the file, and frees each one when
  // written. The USB thread waits only when every block is still waiting for
  // the card -- and then for one block's write.
  namespace card
  {
    constexpr unsigned ring_blocks = 16;
    constexpr std::uint32_t block_bytes = 32 * 1024;

    // 512 KiB, cache-line aligned: the USB core DMAs into the blocks.
    alignas (64) std::uint8_t s_ring[ring_blocks][block_bytes];
    semaphore_counting s_free{ "ring", ring_blocks, ring_blocks };
    unsigned s_next = 0;

    struct request
    {
      const std::uint8_t* data; // nullptr: stop
      std::uint32_t len;
    };
    message_queue_inclusive<request, ring_blocks + 1> s_queue{ "card" };
    std::uint32_t s_errors = 0;
    std::uint32_t s_writes = 0;
    std::uint32_t s_max_write_us = 0;

    void*
    writer (void*)
    {
      for (;;)
        {
          request r{};
          s_queue.receive (&r);
          if (r.data == nullptr)
            return nullptr;
          if (r.len != 0)
            {
              const std::uint32_t t0 = now_us ();
              if (!fatfshw::append_file (usb_leaf, r.data, r.len))
                ++s_errors;
              const std::uint32_t dt = now_us () - t0;
              if (dt > s_max_write_us)
                s_max_write_us = dt;
              ++s_writes;
            }
          s_free.post ();
        }
    }

    thread::stack::element_t s_stack[2048]; // 16 KiB

    // The next free block (waits while there is none); and how long it waited.
    std::uint8_t*
    next_block (std::uint32_t& waited_us)
    {
      const std::uint32_t t0 = now_us ();
      s_free.wait ();
      waited_us = now_us () - t0;
      // Written in the order handed over, so they come free in that order.
      std::uint8_t* b = s_ring[s_next];
      s_next = (s_next + 1) % ring_blocks;
      return b;
    }

    // A filled block to the writer; returns at once.
    void
    submit_block (const std::uint8_t* block, std::uint32_t len)
    {
      const request r{ block, len };
      s_queue.send (&r);
    }

    // Every block back: every write done. True when none failed.
    bool
    flush (void)
    {
      for (unsigned k = 0; k < ring_blocks; ++k)
        s_free.wait ();
      for (unsigned k = 0; k < ring_blocks; ++k)
        s_free.post ();
      return s_errors == 0;
    }
  } // namespace card

  // ---- The transfer --------------------------------------------------------
  namespace big
  {
    constexpr std::uint8_t cmd_big = 0x02;
    constexpr std::uint32_t chunk = 16 * 1024; // one ep_read (), one echo

    // DMA buffers: cache-line aligned, sizes multiples of the 512-byte packet.
    alignas (64) std::uint8_t s_header[512];
    alignas (64) std::uint8_t s_ack[64];

    struct result
    {
      bool done;
      std::uint32_t announced, received, echoed, crc, blocks, us, max_wait_us,
          bad_headers;
      bool stored;
    };
    result s_last{};
    semaphore_binary s_done{ "usb_done", 0 };

    std::uint32_t
    be32 (const std::uint8_t* p)
    {
      return (std::uint32_t{ p[0] } << 24) | (std::uint32_t{ p[1] } << 16)
             | (std::uint32_t{ p[2] } << 8) | p[3];
    }

    void
    transfer (std::uint32_t len)
    {
      result r{};
      r.announced = len;
      const std::uint32_t t0 = now_us ();
      std::uint32_t waited = 0;
      std::uint8_t* block = card::next_block (waited);
      std::uint32_t used = 0;

      while (r.received < len)
        {
          const std::uint32_t left = len - r.received;
          const std::uint32_t want = left < chunk ? left : chunk;
          const int n = usb_dwc2::ep_read (block + used, want); // DMA into the block
          if (n <= 0)
            break;
          const auto got = static_cast<std::uint32_t> (n);
          if (usb_dwc2::ep_write_stream (block + used, got) == n) // the echo, at once: one long stream
            r.echoed += got;
          r.crc = crc32 (r.crc, block + used, got);
          r.received += got;
          used += got;
          // A full block to the card's owner, and on with the next one: the
          // bus is not left waiting for the card.
          if (used == card::block_bytes || (got < want && used != 0))
            {
              card::submit_block (block, used);
              ++r.blocks;
              block = card::next_block (waited);
              if (waited > r.max_wait_us)
                r.max_wait_us = waited;
              used = 0;
            }
        }
      if (used != 0)
        {
          card::submit_block (block, used);
          ++r.blocks;
        }
      else
        card::submit_block (block, 0); // the block taken last, given back unused

      s_ack[0] = cmd_big;
      s_ack[1] = static_cast<std::uint8_t> (r.received >> 24);
      s_ack[2] = static_cast<std::uint8_t> (r.received >> 16);
      s_ack[3] = static_cast<std::uint8_t> (r.received >> 8);
      s_ack[4] = static_cast<std::uint8_t> (r.received);
      s_ack[5] = 'O';
      s_ack[6] = 'K';
      usb_dwc2::ep_write (s_ack, 7);
      r.us = now_us () - t0;
      // The host has everything; now the last blocks to the card.
      r.stored = card::flush ();
      r.done = true;
      r.bad_headers = s_last.bad_headers;
      s_last = r;
      s_done.post ();
    }

    // The USB service thread's work once the host has configured the device.
    void
    serve (void*)
    {
      for (;;)
        {
          const int n = usb_dwc2::ep_read (s_header, sizeof (s_header));
          if (n >= 5 && s_header[0] == cmd_big)
            transfer (be32 (s_header + 1));
          else
            ++s_last.bad_headers;
        }
    }
  } // namespace big

  // ---- The USB device ------------------------------------------------------
  namespace usb
  {
    // The service thread never returns, so its stack is static.
    thread::stack::element_t s_stack[2048]; // 16 KiB

    void*
    service (void*)
    {
      usb_dwc2::service_thread_body (); // never returns
      return nullptr;
    }

    void
    diagnose (void)
    {
      const usb_dwc2::HwInfo hw = usb_dwc2::probe_hw ();
      const volatile usb_dwc2::Debug& d = usb_dwc2::debug ();
      const usb_dwc2::RegSnapshot r = usb_dwc2::snapshot ();
      console ("  usb: snpsid %08lx hwcfg2 %08lx hwcfg3 %08lx hwcfg4 %08lx\n",
               static_cast<unsigned long> (hw.snpsid), static_cast<unsigned long> (hw.hwcfg2),
               static_cast<unsigned long> (hw.hwcfg3), static_cast<unsigned long> (hw.hwcfg4));
      console ("  usb: isr %lu, bus resets %lu, enum done %lu, setups %lu, configured %u\n",
               static_cast<unsigned long> (d.isr_calls), static_cast<unsigned long> (d.usbrst),
               static_cast<unsigned long> (d.enumdone), static_cast<unsigned long> (d.setups),
               usb_dwc2::configured () ? 1u : 0u);
      console ("  usb: dcfg %08lx dctl %08lx dsts %08lx gintsts %08lx gintmsk %08lx gahbcfg %08lx\n",
               static_cast<unsigned long> (r.dcfg), static_cast<unsigned long> (r.dctl),
               static_cast<unsigned long> (r.dsts), static_cast<unsigned long> (r.gintsts),
               static_cast<unsigned long> (r.gintmsk), static_cast<unsigned long> (r.gahbcfg));
    }
  } // namespace usb

  bool g_all_pass = true;

  void
  check (bool ok, const char* what)
  {
    console ("  [%s] %s\n", ok ? "ok" : "BAD", what);
    if (!ok)
      g_all_pass = false;
  }

  [[noreturn]] void
  finish (const char* summary)
  {
    if (g_all_pass)
      {
        console ("RESULT: PASS (%s)\n", summary);
        hw_result::ok ();
      }
    else
      {
        console ("RESULT: FAIL (see above)\n");
        hw_result::fail ();
      }
    for (;;)
      sysclock.sleep_for (1000);
  }

  // The file read back: its size and CRC-32.
  bool
  file_crc (std::uint32_t& size, std::uint32_t& crc)
  {
    if (!fatfshw::file_size (usb_leaf, size))
      return false;
    crc = 0;
    static std::uint8_t buf[card::block_bytes];
    for (std::uint32_t off = 0; off < size;)
      {
        const std::uint32_t n = size - off < sizeof (buf) ? size - off : sizeof (buf);
        if (!fatfshw::load_file_at (usb_leaf, off, buf, n))
          return false;
        crc = crc32 (crc, buf, n);
        off += n;
      }
    return true;
  }
} // namespace

// ---------------------------------------------------------------------------
int
os_main (int, char*[])
{
  console ("\n"
           "+=================================================+\n"
           "|  " PORT_BANNER_LONG " - micro-os-plus-iii      |\n"
           "|  USB --big transfer: echo, and the file on SD   |\n"
           "+=================================================+\n");

#if !defined(HW_BUILD)
  // QEMU's raspi4b models the DWC2 core as a host controller only, and the
  // boot card's FAT32 is the board's: nothing to do here (as usb_test).
  console ("\nRESULT: SKIP (QEMU: no device-mode DWC2, no boot card)\n");
  hw_result::ok ();
  for (;;)
    sysclock.sleep_for (1000);
#endif

  // ---- The card ------------------------------------------------------------
  static sd::SdCard sdcard;
  if (!sdcard.init ())
    {
      console ("  SD init FAILED: %s\n", sdcard.last_error ());
      check (false, "the SD card is ready");
      finish ("");
    }
  fatfshw::bind_card (sdcard);
  check (fatfshw::mount_volume (), "the FAT32 volume is mounted");
  check (fatfshw::ensure_tests_dir (), "tests/ is there");
  // Created empty: the blocks are appended (creating it takes time the bus
  // does not have).
  check (fatfshw::store_file (usb_leaf, "", 0), "tests/usbfile.bin created empty");

  thread::attributes wa = attributes_for (prio_card, card::s_stack, sizeof (card::s_stack));
  static thread writer{ "card", card::writer, nullptr, wa };

  // ---- The USB device --------------------------------------------------------
  console ("\n-- the USB device: the DWC2 gadget behind the USB-C socket, 1d6b:0104\n");
  usb_dwc2::set_configured_callback (big::serve, nullptr);
  const usb_dwc2::InitResult ir = usb_dwc2::init ();
  if (ir != usb_dwc2::InitResult::ok)
    {
      console ("  not up: %s\n", usb_dwc2::init_result_str (ir));
      check (false, "the USB device is up");
      finish ("");
    }
  // At once: the host may enumerate as soon as init () released the soft
  // disconnect, and only this thread answers it.
  thread::attributes ua = attributes_for (prio_usb, usb::s_stack, sizeof (usb::s_stack));
  static thread service{ "usb", usb::service, nullptr, ua };
  check (true, "the USB device is up, its service thread on core 0");
  console ("  the host may send: host/output/usb_test_host --big <file>\n");

  // ---- The transfer ----------------------------------------------------------
  console ("\n-- the USB transfer: waiting for the host (at most %u s)\n", usb_wait_s);
  const bool done = big::s_done.timed_wait (usb_wait_s * 1000u) == result::ok;
  const big::result& u = big::s_last;
  if (done)
    console ("  %lu B announced, %lu received, %lu echoed, in %lu ms; %lu blocks to the card "
             "(waited %lu us at most; %lu writes, the longest %lu us); crc %08lx\n",
             static_cast<unsigned long> (u.announced), static_cast<unsigned long> (u.received),
             static_cast<unsigned long> (u.echoed), static_cast<unsigned long> (u.us / 1000),
             static_cast<unsigned long> (u.blocks), static_cast<unsigned long> (u.max_wait_us),
             static_cast<unsigned long> (card::s_writes), static_cast<unsigned long> (card::s_max_write_us),
             static_cast<unsigned long> (u.crc));
  else
    usb::diagnose ();
  check (done, "the host's file arrived");
  check (done && u.received == u.announced && u.echoed == u.received && u.stored,
         "every byte received, echoed and stored");

  // ---- The read-back ---------------------------------------------------------
  card::submit_block (nullptr, 0); // the writer returns
  writer.join ();
  console ("\n-- the card read back\n");
  std::uint32_t size = 0;
  std::uint32_t crc = 0;
  const bool read = file_crc (size, crc);
  console ("  tests/%s: %lu B, crc %08lx\n", usb_leaf, static_cast<unsigned long> (size),
           static_cast<unsigned long> (crc));
  check (read && done && size == u.received && crc == u.crc,
         "the file on the card is the one received (size and CRC-32)");

  finish ("the host's file echoed and stored on the SD card");
}

// ---------------------------------------------------------------------------
// µOS++ start-up (as sd_test)
// ---------------------------------------------------------------------------
extern "C"
{
  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware_early (void) { }

  void
  os_startup_initialize_hardware (void)
  {
    uart::uart1.init ();
    os_startup_initialize_free_store (
        __heap_start, static_cast<std::size_t> (__heap_end - __heap_start));
    exception::init ();
  }

  [[noreturn]] static void
  custom_main_trampoline (void)
  {
    std::exit (os_main (0, nullptr));
  }

  extern void os_startup_create_thread_idle (void);
  extern os::rtos::thread* os_main_thread;

  int
  main (int, char*[])
  {
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack ()->set (
        reinterpret_cast<os::rtos::thread::stack::element_t*> (__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack ()->initialize ();
#endif
    scheduler::initialize ();

    static thread::stack::element_t main_stack[32768];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof (main_stack);
#if defined(OS_USE_SMP_SCHEDULER)
    attr.th_cpu_affinity = 1u << 0;
#endif
    static thread main_thread {
      "main", reinterpret_cast<thread::func_t> (custom_main_trampoline),
      nullptr, attr
    };
    os_main_thread = &main_thread;

    os_startup_create_thread_idle ();
    scheduler::start ();
    return 0;
  }
}
