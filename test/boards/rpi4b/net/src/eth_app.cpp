// eth_app.cpp - the shared bring-up script and the SD card thread.
#include "eth_app.hpp"

#include "genet.hpp"
#include "net.hpp"
#include "platform.hpp"

#include <cmsis-plus/rtos/os.h>

#include <fatfs_hw.hpp>
#include <sd.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace os::rtos;

namespace rpi4b::eth_app
{
  namespace
  {
    // ------------------------------------------------------------ RAM log
    char g_log[8192];
    std::uint32_t g_log_size = 0;

    char g_detail[160] = "";

    // ---------------------------------------------------------- SD thread
    alignas (16) std::uint8_t g_sd_stack[64 * 1024];
    semaphore_binary g_sd_work{ "sd-work", 0 };

    volatile bool g_sd_ready = false;
    volatile bool g_sd_failed = false;
    volatile bool g_sd_busy = false;
    volatile bool g_sd_ok = false;

    const char* g_req_leaf = nullptr;
    const void* g_req_data = nullptr;
    std::uint32_t g_req_size = 0;

    // Read-back buffer for the comparison, one cluster-friendly piece at a
    // time so a multi-megabyte file needs no second copy in RAM.
    std::uint8_t g_verify[64 * 1024];

    bool
    store_and_verify (const char* leaf, const void* data, std::uint32_t size)
    {
      (void)fatfshw::remove_file (leaf);
      if (!fatfshw::store_file (leaf, data, size))
        {
          kprintf ("[SD] write tests/%s failed: %s\n", leaf,
                   fatfshw::last_error ());
          return false;
        }
      std::uint32_t stored = 0;
      if (!fatfshw::file_size (leaf, stored) || stored != size)
        {
          kprintf ("[SD] tests/%s is %u bytes, expected %u\n", leaf,
                   static_cast<unsigned> (stored),
                   static_cast<unsigned> (size));
          return false;
        }
      const auto* expected = static_cast<const std::uint8_t*> (data);
      for (std::uint32_t at = 0; at < size;)
        {
          const std::uint32_t piece
              = size - at < sizeof (g_verify) ? size - at : sizeof (g_verify);
          if (!fatfshw::load_file_at (leaf, at, g_verify, piece)
              || std::memcmp (g_verify, expected + at, piece) != 0)
            {
              kprintf ("[SD] tests/%s differs at offset %u\n", leaf,
                       static_cast<unsigned> (at));
              return false;
            }
          at += piece;
        }
      kprintf ("[SD] saved tests/%s, %u bytes, read back and verified\n",
               leaf, static_cast<unsigned> (size));
      return true;
    }

    void*
    sd_func (void*)
    {
      static sd::SdCard card;
      if (!card.init ())
        {
          kprintf ("[SD] card init failed: %s\n", card.last_error ());
          g_sd_failed = true;
        }
      else
        {
          fatfshw::bind_card (card);
          if (!fatfshw::mount_volume () || !fatfshw::ensure_tests_dir ())
            {
              kprintf ("[SD] mount failed: %s\n", fatfshw::last_error ());
              g_sd_failed = true;
            }
          else
            {
              kprintf ("[SD] card ready: %u MiB, FAT volume mounted, files "
                       "go to tests/\n",
                       static_cast<unsigned> (card.capacity_bytes () >> 20));
            }
        }
      g_sd_ready = true;

      for (;;)
        {
          g_sd_work.wait ();
          g_sd_ok = !g_sd_failed
                    && store_and_verify (g_req_leaf, g_req_data, g_req_size);
          g_sd_busy = false;
        }
      return nullptr;
    }
  } // namespace

  void
  log (const char* format, ...) noexcept
  {
    if (g_log_size >= sizeof (g_log) - 1)
      {
        return;
      }
    std::va_list args;
    va_start (args, format);
    const int n = std::vsnprintf (g_log + g_log_size,
                                  sizeof (g_log) - g_log_size, format, args);
    va_end (args);
    if (n > 0)
      {
        g_log_size += static_cast<std::uint32_t> (n);
        if (g_log_size > sizeof (g_log) - 1)
          {
            g_log_size = sizeof (g_log) - 1;
          }
      }
  }

  const char*
  log_text () noexcept
  {
    return g_log;
  }

  std::uint32_t
  log_size () noexcept
  {
    return g_log_size;
  }

  void
  serve_for (std::uint32_t ms) noexcept
  {
    const std::uint32_t start = platform::millis ();
    do
      {
        net::poll ();
        sysclock.sleep_for (2);
      }
    while (platform::millis () - start < ms);
  }

  bring_up_result
  bring_up (const char* static_address, const char* static_gateway) noexcept
  {
    bring_up_result result{};
    const bool have_static = static_address != nullptr
                             && static_address[0] != '\0';

    kprintf ("\n[NET] bringing up the GENET controller ...\n");
    if (!genet::start ())
      {
        kprintf ("[NET] controller bring-up FAILED (GENET v%u, PHY id "
                 "%08x)\n",
                 genet::version (), static_cast<unsigned> (genet::phy_id ()));
        log ("controller: FAILED\n");
        std::snprintf (g_detail, sizeof (g_detail),
                       "no GENET controller or PHY (GENET v%u, PHY id %08x)",
                       genet::version (),
                       static_cast<unsigned> (genet::phy_id ()));
        return result;
      }
    result.controller = true;
    const auto& mac = genet::address ();
    kprintf ("[NET] GENET v%u, PHY id %08x, MAC %02x:%02x:%02x:%02x:%02x:%02x "
             "(%s)\n",
             genet::version (), static_cast<unsigned> (genet::phy_id ()),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             genet::address_source ());
    log ("controller: GENET v%u, PHY id %08x\n", genet::version (),
         static_cast<unsigned> (genet::phy_id ()));
    log ("mac: %02x:%02x:%02x:%02x:%02x:%02x (%s)\n", mac[0], mac[1], mac[2],
         mac[3], mac[4], mac[5], genet::address_source ());

    net::init ();
    if (have_static)
      {
        net::set_static_address (static_address, static_gateway);
      }

    // Auto-negotiation takes two to three seconds on a gigabit link.
    kprintf ("[NET] waiting for the link ...\n");
    const std::uint32_t start = platform::millis ();
    // First with gigabit in the offer; if that does not settle in 7 s, with
    // 10/100 only, which needs less of the cable and of the partner.
    bool gigabit_offered = true;
    char first_try[64] = "";
    while (!net::link_up () && platform::millis () - start < 15000u)
      {
        net::poll ();
        if (gigabit_offered && platform::millis () - start >= 7000u)
          {
            genet::phy_summary (first_try, sizeof (first_try));
            kprintf ("[NET] no link at 1000 Mbit/s (%s); offering 10/100 "
                     "only\n",
                     first_try);
            log ("link: not with gigabit offered (%s)\n", first_try);
            genet::renegotiate (false);
            gigabit_offered = false;
          }
        sysclock.sleep_for (2);
      }
    if (!net::link_up ())
      {
        kprintf ("[NET] no link within 15 s: is the cable plugged in?\n");
        char second_try[64] = "";
        genet::phy_summary (second_try, sizeof (second_try));
        std::snprintf (g_detail, sizeof (g_detail),
                       "no Ethernet link; 1000: %s; 10/100: %s", first_try,
                       second_try);
        genet::print_phy ();
        // Without a cable the controller can still be shown to work: one
        // frame looped in the MAC, one looped in the PHY (across RGMII).
        const bool mac_loop = genet::loopback_test (false);
        const bool phy_loop = genet::loopback_test (true);
        kprintf ("[NET] loopback self-test: MAC %s, PHY %s\n",
                 mac_loop ? "ok" : "FAILED", phy_loop ? "ok" : "FAILED");
        log ("loopback: MAC %s, PHY %s\n", mac_loop ? "ok" : "FAILED",
             phy_loop ? "ok" : "FAILED");
        log ("link: DOWN\n");
        return result;
      }
    result.link = true;
    log ("link: up, %u Mbit/s %s duplex, after %u ms\n", genet::speed (),
         genet::full_duplex () ? "full" : "half",
         static_cast<unsigned> (platform::millis () - start));

    if (!have_static)
      {
        if (!net::wait_for_address (20000))
          {
            kprintf ("[NET] no DHCP lease within 20 s\n");
            std::snprintf (g_detail, sizeof (g_detail),
                           "no DHCP lease; link %u Mbit/s, rx %u frames, tx "
                           "%u frames",
                           genet::speed (),
                           static_cast<unsigned> (genet::stats ().rx_frames),
                           static_cast<unsigned> (genet::stats ().tx_frames));
            log ("address: none (no DHCP lease)\n");
            return result;
          }
      }
    else
      {
        serve_for (200);
      }
    result.address = net::has_address ();
    result.leased = net::leased ();
    if (result.address)
      {
        const std::uint32_t ip = net::address ();
        const std::uint32_t gw = net::gateway ();
        net::print_status ();
        log ("address: %u.%u.%u.%u (%s), gateway %u.%u.%u.%u\n",
             static_cast<unsigned> (ip >> 24),
             static_cast<unsigned> ((ip >> 16) & 0xff),
             static_cast<unsigned> ((ip >> 8) & 0xff),
             static_cast<unsigned> (ip & 0xff),
             result.leased ? "DHCP" : "static",
             static_cast<unsigned> (gw >> 24),
             static_cast<unsigned> ((gw >> 16) & 0xff),
             static_cast<unsigned> ((gw >> 8) & 0xff),
             static_cast<unsigned> (gw & 0xff));
      }
    return result;
  }

  const char*
  failure_detail () noexcept
  {
    return g_detail;
  }

  void
  saver_start () noexcept
  {
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = g_sd_stack;
    attr.th_stack_size_bytes = sizeof (g_sd_stack);
    attr.th_priority = thread::priority::normal;
    static thread sd_thread{ "sd", sd_func, nullptr, attr };
  }

  bool
  saver_ready () noexcept
  {
    return g_sd_ready;
  }

  bool
  saver_failed () noexcept
  {
    return g_sd_failed;
  }

  bool
  saver_request (const char* leaf, const void* data,
                 std::uint32_t size) noexcept
  {
    if (g_sd_busy)
      {
        return false;
      }
    g_req_leaf = leaf;
    g_req_data = data;
    g_req_size = size;
    g_sd_ok = false;
    g_sd_busy = true;
    g_sd_work.post ();
    return true;
  }

  bool
  saver_busy () noexcept
  {
    return g_sd_busy;
  }

  bool
  saver_ok () noexcept
  {
    return g_sd_ok;
  }

  bool
  save_and_wait (const char* leaf, const void* data, std::uint32_t size,
                 std::uint32_t timeout_ms) noexcept
  {
    const std::uint32_t start = platform::millis ();
    while (!saver_ready ())
      {
        if (platform::millis () - start >= timeout_ms)
          {
            return false;
          }
        net::poll ();
        sysclock.sleep_for (2);
      }
    if (!saver_request (leaf, data, size))
      {
        return false;
      }
    while (saver_busy ())
      {
        if (platform::millis () - start >= timeout_ms)
          {
            kprintf ("[SD] tests/%s: no completion within %u ms\n", leaf,
                     static_cast<unsigned> (timeout_ms));
            return false;
          }
        net::poll ();
        sysclock.sleep_for (2);
      }
    return saver_ok ();
  }
} // namespace rpi4b::eth_app
