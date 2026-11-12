/*
 * eth-net — the Raspberry Pi 4 B Ethernet (GENET v5 + BCM54213PE) and lwIP
 * under the µOS++ scheduler: link, DHCP, ping, and a log saved to the SD card.
 *
 * The wired counterpart of the Mango Pi MQ-Pro wifi-net test, with the same
 * shape:
 *
 *   thread      prio        stack   period  role
 *   ----------  ----------  ------  ------  -------------------------------
 *   net         above_norm  32 KiB  2 ms    OWNS the controller and lwIP:
 *                                           runs the scripted bring-up, the
 *                                           DHCP wait and the pings, and
 *                                           polls the receive ring between
 *                                           steps.
 *   sd          normal      64 KiB  -       OWNS the SD card and FatFs:
 *                                           mounts the boot card's FAT volume
 *                                           and saves a file when asked.
 *   os_main     normal         -      -     starts both, waits for the
 *                                           verdict, prints the summary.
 *
 * Why it is shaped this way
 * -------------------------
 *   * ONE THREAD OWNS THE NETWORK. The driver is polled and lwIP runs
 *     without an OS layer (NO_SYS): neither has any locking. Everything
 *     that touches them, the blocking helpers included (net::ping pumps the
 *     stack while it waits), runs on the net thread.
 *
 *   * THE POLL PERIOD IS 2 ms. The receive ring has 256 buffers, so a burst
 *     is not lost between two polls, lwIP's timers are serviced far below
 *     their 250 ms granularity, and the thread sleeps most of the time.
 *
 *   * THE SD CARD HAS ITS OWN THREAD. A FAT write blocks for milliseconds
 *     to seconds; the net thread posts the request and keeps polling until
 *     it completes, so the link is serviced while the card is busy.
 *
 * Verdict: PASS when the controller and the PHY answer, the link comes up,
 * the interface gets an address (DHCP, or the static one given at configure
 * time), the gateway answers an ICMP echo, and the run's log is written to
 * tests/ETHNET.LOG on the SD card and read back unchanged.
 *
 * Configure-time options (cmake -D NAME=value, see tests.cmake):
 *   ETH_TEST_STATIC_IP   "a.b.c.d/len": use this address instead of DHCP
 *   ETH_TEST_GATEWAY     gateway to ping; default: the one DHCP gave
 *   ETH_TEST_PEER        optional second host to ping
 */

#include <cmsis-plus/rtos/os.h>

#include <hw_result.hpp>
#include <uart.hpp>

#include "eth_app.hpp"
#include "genet.hpp"
#include "net.hpp"
#include "platform.hpp"

#include <cstdint>

#ifndef ETH_TEST_STATIC_IP
#define ETH_TEST_STATIC_IP ""
#endif
#ifndef ETH_TEST_GATEWAY
#define ETH_TEST_GATEWAY ""
#endif
#ifndef ETH_TEST_PEER
#define ETH_TEST_PEER ""
#endif

using namespace os::rtos;
namespace app = rpi4b::eth_app;
namespace net = rpi4b::net;
namespace genet = rpi4b::genet;
namespace platform = rpi4b::platform;

namespace
{
  alignas (16) std::uint8_t net_stack[32 * 1024];

  semaphore_binary g_done{ "net-done", 0 };
  volatile bool g_pass = false;
  const char* volatile g_reason = "see the log above";

  void*
  net_func (void*)
  {
    const auto up = app::bring_up (ETH_TEST_STATIC_IP, ETH_TEST_GATEWAY);

    // --- ping -------------------------------------------------------------
    unsigned replies = 0;
    if (up.address)
      {
        const char* gateway = ETH_TEST_GATEWAY[0] != '\0' ? ETH_TEST_GATEWAY
                                                         : "gw";
        kprintf ("[NET] ping the gateway ...\n");
        replies = net::ping (gateway, 4);
        app::log ("ping gateway: %u of 4 replies\n", replies);
        if (ETH_TEST_PEER[0] != '\0')
          {
            kprintf ("[NET] ping peer %s ...\n", ETH_TEST_PEER);
            const unsigned peer = net::ping (ETH_TEST_PEER, 2);
            app::log ("ping %s: %u of 2 replies\n", ETH_TEST_PEER, peer);
          }
      }

    // --- counters ---------------------------------------------------------
    const auto& stat = genet::stats ();
    kprintf ("[NET] rx %u frames (%u errors), tx %u frames (%u errors), "
             "%u link change(s)\n",
             static_cast<unsigned> (stat.rx_frames),
             static_cast<unsigned> (stat.rx_errors),
             static_cast<unsigned> (stat.tx_frames),
             static_cast<unsigned> (stat.tx_errors),
             static_cast<unsigned> (stat.link_changes));
    app::log ("frames: rx %u (%u errors), tx %u (%u errors)\n",
              static_cast<unsigned> (stat.rx_frames),
              static_cast<unsigned> (stat.rx_errors),
              static_cast<unsigned> (stat.tx_frames),
              static_cast<unsigned> (stat.tx_errors));

    const bool network = up.controller && up.link && up.address && replies > 0;
    app::log ("network: %s\n", network ? "PASS" : "FAIL");

    // --- save the log -----------------------------------------------------
    // The link stays serviced while the card is written.
    const bool saved = app::save_and_wait ("ETHNET.LOG", app::log_text (),
                                           app::log_size ());

    if (!up.controller || !up.link || !up.address)
      {
        // The bring-up says what stopped it, with the PHY or frame counts.
        g_reason = app::failure_detail ();
      }
    else if (replies == 0)
      {
        g_reason = "the gateway did not answer";
      }
    else if (!saved)
      {
        g_reason = "the log was not saved to the SD card";
      }
    g_pass = network && saved;
    g_done.post ();

    // Keep the interface alive until os_main ends the run.
    for (;;)
      {
        net::poll ();
        sysclock.sleep_for (2);
      }
    return nullptr;
  }
} // namespace

int
os_main (int, char*[])
{
#if !defined(HW_BUILD)
  // The emulated image: QEMU's machine is not asked for a network here, so
  // there is nothing to test. SKIP counts as a pass for the runner.
  kprintf ("\nRESULT: SKIP (%s needs the real GENET controller and a cable)\n",
           "eth-net");
  hw_result::ok ();
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
#endif
  kprintf ("\n=== eth-net: GENET + lwIP on micro-os-plus (" PORT_BANNER_LONG
           ") ===\n"
           "[TEST] threads: net (controller + lwIP, above normal, 32 KiB, "
           "2 ms poll)\n"
           "[TEST]          sd  (SD card + FatFs, normal, 64 KiB, on "
           "request)\n");
  app::log ("eth-net on " PORT_BANNER_LONG "\n");

  // The SD thread first: it mounts the card while the link negotiates.
  app::saver_start ();

  thread::attributes attr = thread::initializer;
  attr.th_stack_address = net_stack;
  attr.th_stack_size_bytes = sizeof (net_stack);
  attr.th_priority = thread::priority::above_normal;
  static thread net_thread{ "net", net_func, nullptr, attr };

  // Bound the run: a silent network must fail, not hang the runner.
  constexpr std::uint32_t timeout_ms = 120 * 1000;
  const std::uint32_t start = platform::millis ();
  while (g_done.try_wait () != result::ok)
    {
      if (platform::millis () - start >= timeout_ms)
        {
          kprintf ("\nRESULT: FAIL (timeout waiting for the network)\n");
          hw_result::fail ();
          for (;;)
            {
              sysclock.sleep_for (1000);
            }
        }
      sysclock.sleep_for (100);
    }

  if (g_pass)
    {
      kprintf ("\nRESULT: PASS (link, address, gateway ping, log on the SD "
               "card)\n");
      hw_result::ok ();
    }
  else
    {
      kprintf ("\nRESULT: FAIL (%s)\n", g_reason);
      hw_result::fail ();
    }
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
  return 0;
}

#include "eth_startup.hpp"
