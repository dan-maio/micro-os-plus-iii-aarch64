// genet.cpp - Broadcom GENET v5 + BCM54213PE on the Raspberry Pi 4 B, polled.
//
// The register layout and the bring-up order follow the U-Boot driver
// (drivers/net/bcmgenet.c), which is the smallest known-good description of
// this controller: one receive ring and one transmit ring (the default
// queue, 16), 256 descriptors each. The descriptors live in the controller's
// own register space; only the packet buffers are in RAM, and those are the
// only memory that needs cache maintenance.
#include "genet.hpp"

#include "platform.hpp"

#include <mailbox.hpp>

#include <cstdio>
#include <cstring>

namespace rpi4b::genet
{
  namespace
  {
    // ------------------------------------------------------------ registers
    constexpr std::uintptr_t base = 0xFD580000u;

    constexpr std::uint32_t SYS_REV_CTRL = 0x000;
    constexpr std::uint32_t SYS_PORT_CTRL = 0x004;
    constexpr std::uint32_t PORT_MODE_EXT_GPHY = 3;
    constexpr std::uint32_t SYS_RBUF_FLUSH_CTRL = 0x008;

    constexpr std::uint32_t EXT_RGMII_OOB_CTRL = 0x08c;
    constexpr std::uint32_t RGMII_LINK = 1u << 4;
    constexpr std::uint32_t OOB_DISABLE = 1u << 5;
    constexpr std::uint32_t RGMII_MODE_EN = 1u << 6;
    constexpr std::uint32_t ID_MODE_DIS = 1u << 16;

    constexpr std::uint32_t INTRL2_CPU_MASK_SET = 0x200 + 0x10;
    constexpr std::uint32_t INTRL2_CPU_CLEAR = 0x200 + 0x08;

    constexpr std::uint32_t RBUF_CTRL = 0x300;
    constexpr std::uint32_t RBUF_ALIGN_2B = 1u << 1;
    constexpr std::uint32_t RBUF_TBUF_SIZE_CTRL = 0x3b4;

    constexpr std::uint32_t UMAC_CMD = 0x808;
    constexpr std::uint32_t CMD_TX_EN = 1u << 0;
    constexpr std::uint32_t CMD_RX_EN = 1u << 1;
    constexpr std::uint32_t CMD_SPEED_SHIFT = 2;
    constexpr std::uint32_t CMD_SPEED_MASK = 3u << CMD_SPEED_SHIFT;
    constexpr std::uint32_t CMD_HD_EN = 1u << 10;
    constexpr std::uint32_t CMD_SW_RESET = 1u << 13;
    constexpr std::uint32_t CMD_LCL_LOOP_EN = 1u << 15;
    constexpr std::uint32_t UMAC_MAC0 = 0x80c;
    constexpr std::uint32_t UMAC_MAC1 = 0x810;
    constexpr std::uint32_t UMAC_MAX_FRAME_LEN = 0x814;
    constexpr std::uint32_t UMAC_TX_FLUSH = 0xb34;
    constexpr std::uint32_t UMAC_MIB_CTRL = 0xd80;
    constexpr std::uint32_t MIB_RESET_RX = 1u << 0;
    constexpr std::uint32_t MIB_RESET_RUNT = 1u << 1;
    constexpr std::uint32_t MIB_RESET_TX = 1u << 2;

    constexpr std::uint32_t MDIO_CMD = 0xe14;
    constexpr std::uint32_t MDIO_START_BUSY = 1u << 29;
    constexpr std::uint32_t MDIO_READ_FAIL = 1u << 28;
    constexpr std::uint32_t MDIO_RD = 2u << 26;
    constexpr std::uint32_t MDIO_WR = 1u << 26;
    constexpr std::uint32_t MDIO_PMD_SHIFT = 21;
    constexpr std::uint32_t MDIO_REG_SHIFT = 16;

    // DMA: 256 descriptors of 12 bytes, then 17 ring register blocks of
    // 0x40 bytes (rings 0..15 and the default queue 16), then the global
    // DMA registers.
    constexpr std::uint32_t descriptors = 256;
    constexpr std::uint32_t desc_size = 12;
    constexpr std::uint32_t ring_size = 0x40;
    constexpr std::uint32_t default_queue = 16;

    constexpr std::uint32_t RX_DESC = 0x2000;
    constexpr std::uint32_t TX_DESC = 0x4000;
    constexpr std::uint32_t RDMA_RING = RX_DESC + descriptors * desc_size
                                        + default_queue * ring_size;
    constexpr std::uint32_t TDMA_RING = TX_DESC + descriptors * desc_size
                                        + default_queue * ring_size;
    constexpr std::uint32_t RDMA_REG = RDMA_RING + ring_size;
    constexpr std::uint32_t TDMA_REG = TDMA_RING + ring_size;

    // Per-ring registers. The index pair is swapped between the two
    // directions: software produces on transmit and consumes on receive.
    constexpr std::uint32_t RING_PTR_A = 0x00; // TDMA read ptr / RDMA write ptr
    constexpr std::uint32_t TDMA_CONS_INDEX = 0x08;
    constexpr std::uint32_t TDMA_PROD_INDEX = 0x0c;
    constexpr std::uint32_t RDMA_PROD_INDEX = 0x08;
    constexpr std::uint32_t RDMA_CONS_INDEX = 0x0c;
    constexpr std::uint32_t RING_BUF_SIZE = 0x10;
    constexpr std::uint32_t RING_START_ADDR = 0x14;
    constexpr std::uint32_t RING_END_ADDR = 0x1c;
    constexpr std::uint32_t RING_MBUF_DONE_THRESH = 0x24;
    constexpr std::uint32_t RING_FLOW_OR_XOFF = 0x28; // TDMA flow period / RDMA xon-xoff
    constexpr std::uint32_t RING_PTR_B = 0x2c; // TDMA write ptr / RDMA read ptr

    // Global DMA registers.
    constexpr std::uint32_t DMA_RING_CFG = 0x00;
    constexpr std::uint32_t DMA_CTRL = 0x04;
    constexpr std::uint32_t DMA_SCB_BURST_SIZE = 0x0c;
    constexpr std::uint32_t DMA_EN = 1u << 0;
    constexpr std::uint32_t DMA_RING_BUF_EN_SHIFT = 1;

    // Descriptor words.
    constexpr std::uint32_t DESC_LENGTH_STATUS = 0;
    constexpr std::uint32_t DESC_ADDRESS_LO = 4;
    constexpr std::uint32_t DESC_ADDRESS_HI = 8;
    constexpr std::uint32_t DMA_BUFLENGTH_SHIFT = 16;
    constexpr std::uint32_t DMA_OWN = 0x8000;
    constexpr std::uint32_t DMA_EOP = 0x4000;
    constexpr std::uint32_t DMA_SOP = 0x2000;
    constexpr std::uint32_t DMA_TX_APPEND_CRC = 0x0040;
    constexpr std::uint32_t DMA_TX_QTAG = 0x3fu << 7;
    constexpr std::uint32_t DMA_RX_ERRORS = 0x001f; // CRC, overrun, frame, length, lg

    constexpr std::uint32_t buffer_length = 2048;
    constexpr std::uint32_t rx_offset = 2; // RBUF_ALIGN_2B: IP header on 4 bytes
    constexpr std::uint32_t max_frame = 1536;

    // ------------------------------------------------------------------ PHY
    constexpr unsigned phy_address = 1; // BCM54213PE on the Pi 4 B
    constexpr unsigned MII_BMCR = 0x00;
    constexpr unsigned MII_BMSR = 0x01;
    constexpr unsigned MII_PHYSID1 = 0x02;
    constexpr unsigned MII_PHYSID2 = 0x03;
    constexpr unsigned MII_ADVERTISE = 0x04;
    constexpr unsigned MII_LPA = 0x05;
    constexpr unsigned MII_CTRL1000 = 0x09;
    constexpr unsigned MII_STAT1000 = 0x0a;
    constexpr unsigned MII_BCM54XX_AUX_CTL = 0x18;
    constexpr unsigned MII_BCM54XX_SHD = 0x1c;
    constexpr std::uint16_t BMCR_RESET = 0x8000;
    constexpr std::uint16_t BMCR_ANENABLE = 0x1000;
    constexpr std::uint16_t BMCR_ANRESTART = 0x0200;
    constexpr std::uint16_t BMSR_LSTATUS = 0x0004;
    constexpr std::uint16_t BMSR_ANEGCOMPLETE = 0x0020;

    // ---------------------------------------------------------------- state
    alignas (64) std::uint8_t rx_buffers[descriptors * buffer_length];
    alignas (64) std::uint8_t tx_buffer[buffer_length];

    mac_address g_address{};
    const char* g_address_source = "local";
    receive_handler g_on_frame = nullptr;
    link_handler g_on_link = nullptr;
    statistics g_stats{};

    bool g_started = false;
    bool g_linked = false;
    unsigned g_speed = 0;
    bool g_full_duplex = false;
    std::uint32_t g_phy_id = 0;
    unsigned g_version = 0;

    std::uint32_t g_rx_index = 0;      // next receive descriptor
    std::uint32_t g_rx_consumed = 0;   // 16-bit consumer index
    std::uint32_t g_tx_index = 0;      // next transmit descriptor
    std::uint32_t g_tx_produced = 0;   // 16-bit producer index
    std::uint32_t g_link_checked_ms = 0;

    inline std::uint32_t
    rd (std::uint32_t offset) noexcept
    {
      return *reinterpret_cast<volatile std::uint32_t*> (base + offset);
    }

    inline void
    wr (std::uint32_t offset, std::uint32_t value) noexcept
    {
      *reinterpret_cast<volatile std::uint32_t*> (base + offset) = value;
    }

    // ----------------------------------------------------------------- MDIO
    bool
    mdio_wait () noexcept
    {
      for (unsigned i = 0; i < 2000; ++i)
        {
          if ((rd (MDIO_CMD) & MDIO_START_BUSY) == 0)
            {
              return true;
            }
          platform::delay_us (10);
        }
      return false;
    }

    int
    mdio_read (unsigned reg) noexcept
    {
      wr (MDIO_CMD, MDIO_RD | (phy_address << MDIO_PMD_SHIFT)
                        | (reg << MDIO_REG_SHIFT));
      wr (MDIO_CMD, rd (MDIO_CMD) | MDIO_START_BUSY);
      if (!mdio_wait ())
        {
          return -1;
        }
      const std::uint32_t value = rd (MDIO_CMD);
      if ((value & MDIO_READ_FAIL) != 0)
        {
          return -1;
        }
      return static_cast<int> (value & 0xffffu);
    }

    bool
    mdio_write (unsigned reg, std::uint16_t value) noexcept
    {
      wr (MDIO_CMD, MDIO_WR | (phy_address << MDIO_PMD_SHIFT)
                        | (reg << MDIO_REG_SHIFT) | value);
      wr (MDIO_CMD, rd (MDIO_CMD) | MDIO_START_BUSY);
      return mdio_wait ();
    }

    // The board is wired "rgmii-rxid": the PHY delays the receive clock, the
    // MAC delays the transmit clock. Two Broadcom shadow registers say so.
    void
    phy_configure_delays () noexcept
    {
      // Auxiliary control, shadow 7 (miscellaneous): RGMII RXC skew on.
      mdio_write (MII_BCM54XX_AUX_CTL, 0x7007);
      int value = mdio_read (MII_BCM54XX_AUX_CTL);
      if (value >= 0)
        {
          mdio_write (MII_BCM54XX_AUX_CTL,
                      static_cast<std::uint16_t> (
                          (value & 0x0ff8) | 0x8000 | 0x0100 | 0x0007));
        }
      // Shadow 0x1c, selector 3 (clock alignment): GTXCLK delay off.
      mdio_write (MII_BCM54XX_SHD, 0x03u << 10);
      value = mdio_read (MII_BCM54XX_SHD);
      if (value >= 0)
        {
          mdio_write (MII_BCM54XX_SHD,
                      static_cast<std::uint16_t> (
                          0x8000 | (0x03u << 10) | (value & 0x03ff & ~0x0200)));
        }
    }

    bool
    phy_start () noexcept
    {
      const int id1 = mdio_read (MII_PHYSID1);
      const int id2 = mdio_read (MII_PHYSID2);
      if (id1 < 0 || id2 < 0 || id1 == 0xffff)
        {
          return false;
        }
      g_phy_id = (static_cast<std::uint32_t> (id1) << 16)
                 | static_cast<std::uint32_t> (id2);

      mdio_write (MII_BMCR, BMCR_RESET);
      for (unsigned i = 0; i < 500; ++i)
        {
          const int bmcr = mdio_read (MII_BMCR);
          if (bmcr >= 0 && (bmcr & BMCR_RESET) == 0)
            {
              break;
            }
          platform::delay_us (1000);
        }
      phy_configure_delays ();

      // Advertise 10/100 half and full, flow control, and 1000 full.
      mdio_write (MII_ADVERTISE, 0x0001 | 0x01e0 | 0x0400);
      mdio_write (MII_CTRL1000, 0x0200);
      mdio_write (MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
      return true;
    }

    // ------------------------------------------------------------------ MAC
    void
    read_firmware_address () noexcept
    {
      // The firmware knows the board's address (OTP); property tag
      // 0x00010003, "get board MAC address".
      alignas (16) static std::uint32_t request[8];
      request[0] = sizeof (request);
      request[1] = 0;
      request[2] = 0x00010003u;
      request[3] = 6;
      request[4] = 0;
      request[5] = 0;
      request[6] = 0;
      request[7] = 0;
      if (mailbox::property_call (request)
          && request[1] == mailbox::kRespSuccess)
        {
          std::memcpy (g_address.data (), &request[5], g_address.size ());
          bool zero = true;
          for (const std::uint8_t byte : g_address)
            {
              zero = zero && byte == 0;
            }
          if (!zero)
            {
              g_address_source = "firmware";
              return;
            }
        }

      // What the boot ROM left in the controller, if anything.
      const std::uint32_t mac0 = rd (UMAC_MAC0);
      const std::uint32_t mac1 = rd (UMAC_MAC1);
      if (mac0 != 0 || (mac1 & 0xffffu) != 0)
        {
          g_address = { static_cast<std::uint8_t> (mac0 >> 24),
                        static_cast<std::uint8_t> (mac0 >> 16),
                        static_cast<std::uint8_t> (mac0 >> 8),
                        static_cast<std::uint8_t> (mac0),
                        static_cast<std::uint8_t> (mac1 >> 8),
                        static_cast<std::uint8_t> (mac1) };
          g_address_source = "controller";
          return;
        }

      // A locally administered address, so the test can still run.
      g_address = { 0x02, 0x52, 0x50, 0x49, 0x34, 0x42 };
      g_address_source = "local";
    }

    void
    umac_reset () noexcept
    {
      std::uint32_t value = rd (SYS_RBUF_FLUSH_CTRL);
      wr (SYS_RBUF_FLUSH_CTRL, value | (1u << 1));
      platform::delay_us (10);
      wr (SYS_RBUF_FLUSH_CTRL, value & ~(1u << 1));
      platform::delay_us (10);
      wr (SYS_RBUF_FLUSH_CTRL, 0);
      platform::delay_us (10);

      wr (UMAC_CMD, 0);
      wr (UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
      platform::delay_us (2);
      wr (UMAC_CMD, 0);

      wr (UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT);
      wr (UMAC_MIB_CTRL, 0);
      wr (UMAC_MAX_FRAME_LEN, max_frame);

      // Two bytes of padding in front of every received frame, so the IP
      // header that follows the 14-byte Ethernet header is word aligned.
      wr (RBUF_CTRL, rd (RBUF_CTRL) | RBUF_ALIGN_2B);
      wr (RBUF_TBUF_SIZE_CTRL, 1);
    }

    void
    write_address () noexcept
    {
      wr (UMAC_MAC0, (static_cast<std::uint32_t> (g_address[0]) << 24)
                         | (static_cast<std::uint32_t> (g_address[1]) << 16)
                         | (static_cast<std::uint32_t> (g_address[2]) << 8)
                         | g_address[3]);
      wr (UMAC_MAC1, (static_cast<std::uint32_t> (g_address[4]) << 8)
                         | g_address[5]);
    }

    void
    dma_disable () noexcept
    {
      wr (TDMA_REG + DMA_CTRL, rd (TDMA_REG + DMA_CTRL) & ~DMA_EN);
      wr (RDMA_REG + DMA_CTRL, rd (RDMA_REG + DMA_CTRL) & ~DMA_EN);
      wr (UMAC_TX_FLUSH, 1);
      platform::delay_us (10);
      wr (UMAC_TX_FLUSH, 0);
    }

    void
    rings_init () noexcept
    {
      const std::uint32_t end = descriptors * desc_size / 4 - 1;

      // Receive: every descriptor owns one 2 KiB buffer.
      for (std::uint32_t i = 0; i < descriptors; ++i)
        {
          const auto address
              = reinterpret_cast<std::uintptr_t> (&rx_buffers[i * buffer_length]);
          const std::uint32_t desc = RX_DESC + i * desc_size;
          wr (desc + DESC_ADDRESS_LO, static_cast<std::uint32_t> (address));
          wr (desc + DESC_ADDRESS_HI,
              static_cast<std::uint32_t> (static_cast<std::uint64_t> (address) >> 32));
          wr (desc + DESC_LENGTH_STATUS,
              (buffer_length << DMA_BUFLENGTH_SHIFT) | DMA_OWN);
        }
      platform::dcache_flush (rx_buffers, sizeof (rx_buffers));

      wr (RDMA_REG + DMA_SCB_BURST_SIZE, 8);
      wr (RDMA_RING + RING_START_ADDR, 0);
      wr (RDMA_RING + RING_PTR_B, 0); // read pointer
      wr (RDMA_RING + RING_PTR_A, 0); // write pointer
      wr (RDMA_RING + RING_END_ADDR, end);
      // The producer index cannot be reset: align the consumer on it.
      g_rx_consumed = rd (RDMA_RING + RDMA_PROD_INDEX) & 0xffffu;
      wr (RDMA_RING + RDMA_CONS_INDEX, g_rx_consumed);
      g_rx_index = g_rx_consumed & (descriptors - 1);
      wr (RDMA_RING + RING_BUF_SIZE, (descriptors << 16) | buffer_length);
      wr (RDMA_RING + RING_FLOW_OR_XOFF, (5u << 16) | (descriptors >> 4));
      wr (RDMA_REG + DMA_RING_CFG, 1u << default_queue);

      // Transmit: one frame at a time, from one buffer.
      wr (TDMA_REG + DMA_SCB_BURST_SIZE, 8);
      wr (TDMA_RING + RING_START_ADDR, 0);
      wr (TDMA_RING + RING_PTR_A, 0); // read pointer
      wr (TDMA_RING + RING_PTR_B, 0); // write pointer
      wr (TDMA_RING + RING_END_ADDR, end);
      g_tx_produced = rd (TDMA_RING + TDMA_CONS_INDEX) & 0xffffu;
      wr (TDMA_RING + TDMA_PROD_INDEX, g_tx_produced);
      g_tx_index = g_tx_produced & (descriptors - 1);
      wr (TDMA_RING + RING_MBUF_DONE_THRESH, 1);
      wr (TDMA_RING + RING_FLOW_OR_XOFF, 0);
      wr (TDMA_RING + RING_BUF_SIZE, (descriptors << 16) | buffer_length);
      wr (TDMA_REG + DMA_RING_CFG, 1u << default_queue);

      const std::uint32_t enable
          = (1u << (default_queue + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;
      wr (TDMA_REG + DMA_CTRL, enable);
      wr (RDMA_REG + DMA_CTRL, rd (RDMA_REG + DMA_CTRL) | enable);
    }

    // Tell the MAC what the PHY negotiated.
    void
    apply_link () noexcept
    {
      std::uint32_t oob = rd (EXT_RGMII_OOB_CTRL);
      oob &= ~(OOB_DISABLE | ID_MODE_DIS);
      oob |= RGMII_LINK | RGMII_MODE_EN;
      wr (EXT_RGMII_OOB_CTRL, oob);

      const std::uint32_t code = g_speed == 1000 ? 2u : g_speed == 100 ? 1u : 0u;
      std::uint32_t cmd = rd (UMAC_CMD);
      cmd &= ~(CMD_SPEED_MASK | CMD_HD_EN);
      cmd |= code << CMD_SPEED_SHIFT;
      if (!g_full_duplex)
        {
          cmd |= CMD_HD_EN;
        }
      wr (UMAC_CMD, cmd | CMD_TX_EN | CMD_RX_EN);
    }

    void
    check_link () noexcept
    {
      // The link bit latches low: read twice for the current state.
      (void)mdio_read (MII_BMSR);
      const int bmsr = mdio_read (MII_BMSR);
      if (bmsr < 0)
        {
          return;
        }
      const bool up = (bmsr & BMSR_LSTATUS) != 0
                      && (bmsr & BMSR_ANEGCOMPLETE) != 0;
      if (up == g_linked)
        {
          return;
        }
      if (up)
        {
          const int advertise = mdio_read (MII_ADVERTISE);
          const int partner = mdio_read (MII_LPA);
          const int control1000 = mdio_read (MII_CTRL1000);
          const int status1000 = mdio_read (MII_STAT1000);
          const int common = (advertise >= 0 && partner >= 0)
                                 ? (advertise & partner)
                                 : 0;
          // STAT1000 bits 11/10 are the partner's 1000 full/half, two
          // positions above the matching CTRL1000 advertisement bits.
          const int gigabit = (control1000 >= 0 && status1000 >= 0)
                                  ? (control1000 & (status1000 >> 2))
                                  : 0;
          if ((gigabit & 0x0200) != 0)
            {
              g_speed = 1000;
              g_full_duplex = true;
            }
          else if ((common & 0x0100) != 0)
            {
              g_speed = 100;
              g_full_duplex = true;
            }
          else if ((common & 0x0080) != 0)
            {
              g_speed = 100;
              g_full_duplex = false;
            }
          else if ((common & 0x0040) != 0)
            {
              g_speed = 10;
              g_full_duplex = true;
            }
          else
            {
              g_speed = 10;
              g_full_duplex = false;
            }
          apply_link ();
        }
      else
        {
          g_speed = 0;
        }
      g_linked = up;
      ++g_stats.link_changes;
      if (g_on_link != nullptr)
        {
          g_on_link (up);
        }
    }
  } // namespace

  namespace
  {
    void poll_receive () noexcept;
  } // namespace

  bool
  start () noexcept
  {
    if (g_started)
      {
        return true;
      }
    // SYS_REV_CTRL bits 27:24 hold 6 for GENET v5 (the numbering starts at 1
    // for v1 but skips: v4 reads 4, v5 reads 6).
    const unsigned major = (rd (SYS_REV_CTRL) >> 24) & 0x0fu;
    g_version = major == 6 ? 5 : major == 5 ? 4 : major;
    if (major != 6)
      {
        return false;
      }

    // Polled: no interrupt may be left pending or enabled.
    wr (INTRL2_CPU_MASK_SET, 0xffffffffu);
    wr (INTRL2_CPU_CLEAR, 0xffffffffu);

    wr (SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    read_firmware_address ();
    umac_reset ();
    write_address ();
    dma_disable ();
    rings_init ();

    if (!phy_start ())
      {
        return false;
      }
    // Receive and transmit are enabled when the link comes up (apply_link).
    g_started = true;
    g_link_checked_ms = platform::millis ();
    return true;
  }

  namespace
  {
    void
    poll_receive () noexcept
    {
    for (unsigned budget = 0; budget < descriptors; ++budget)
        {
          const std::uint32_t produced = rd (RDMA_RING + RDMA_PROD_INDEX) & 0xffffu;
          if (produced == g_rx_consumed)
            {
              break;
            }
          const std::uint32_t desc = RX_DESC + g_rx_index * desc_size;
          const std::uint32_t status = rd (desc + DESC_LENGTH_STATUS);
          const std::uint32_t length = (status >> DMA_BUFLENGTH_SHIFT) & 0x0fffu;
          std::uint8_t* buffer = &rx_buffers[g_rx_index * buffer_length];

          const bool whole = (status & (DMA_SOP | DMA_EOP)) == (DMA_SOP | DMA_EOP);
          if (whole && (status & DMA_RX_ERRORS) == 0 && length > rx_offset + 14u
              && length <= buffer_length)
            {
              // The device wrote this memory behind the cache's back.
              platform::dcache_invalidate (buffer, length);
              ++g_stats.rx_frames;
              if (g_on_frame != nullptr)
                {
                  // The FCS is not forwarded (CMD_CRC_FWD is off).
                  g_on_frame ({ buffer + rx_offset, length - rx_offset });
                }
            }
          else
            {
              ++g_stats.rx_errors;
            }

          // Hand the buffer back. Invalidate again: the handler pulled the
          // lines into the cache, and the device will rewrite the memory.
          platform::dcache_invalidate (buffer, buffer_length);
          g_rx_consumed = (g_rx_consumed + 1u) & 0xffffu;
          wr (RDMA_RING + RDMA_CONS_INDEX, g_rx_consumed);
          g_rx_index = (g_rx_index + 1u) & (descriptors - 1);
        }
    }
  } // namespace

  void
  poll () noexcept
  {
    if (!g_started)
      {
        return;
      }
    poll_receive ();

    // The link, a few times a second: MDIO is slow.
    const std::uint32_t now = platform::millis ();
    if (now - g_link_checked_ms >= 250u)
      {
        g_link_checked_ms = now;
        check_link ();
      }
  }

  bool
  send_ethernet (std::span<const std::uint8_t> frame) noexcept
  {
    if (!g_started || !g_linked || frame.size () < 14 || frame.size () > 1514)
      {
        ++g_stats.tx_errors;
        return false;
      }
    // Pad to the 60-byte minimum; the MAC appends the FCS.
    std::size_t length = frame.size ();
    std::memcpy (tx_buffer, frame.data (), length);
    if (length < 60)
      {
        std::memset (tx_buffer + length, 0, 60 - length);
        length = 60;
      }
    platform::dcache_flush (tx_buffer, length);

    const auto address = reinterpret_cast<std::uintptr_t> (tx_buffer);
    const std::uint32_t desc = TX_DESC + g_tx_index * desc_size;
    wr (desc + DESC_ADDRESS_LO, static_cast<std::uint32_t> (address));
    wr (desc + DESC_ADDRESS_HI,
        static_cast<std::uint32_t> (static_cast<std::uint64_t> (address) >> 32));
    wr (desc + DESC_LENGTH_STATUS,
        (static_cast<std::uint32_t> (length) << DMA_BUFLENGTH_SHIFT)
            | DMA_TX_QTAG | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP);

    g_tx_index = (g_tx_index + 1u) & (descriptors - 1);
    g_tx_produced = (g_tx_produced + 1u) & 0xffffu;
    wr (TDMA_RING + TDMA_PROD_INDEX, g_tx_produced);

    // One buffer, so wait until the controller has taken the frame: at
    // 100 Mbit/s a full frame is on the wire in about 120 us.
    const std::uint64_t deadline = platform::time_us () + 100000u;
    while ((rd (TDMA_RING + TDMA_CONS_INDEX) & 0xffffu) != g_tx_produced)
      {
        if (platform::time_us () > deadline)
          {
            ++g_stats.tx_errors;
            return false;
          }
      }
    ++g_stats.tx_frames;
    return true;
  }

  void
  set_receive_handler (receive_handler handler) noexcept
  {
    g_on_frame = handler;
  }

  void
  set_link_handler (link_handler handler) noexcept
  {
    g_on_link = handler;
  }

  const mac_address&
  address () noexcept
  {
    return g_address;
  }

  bool
  linked () noexcept
  {
    return g_linked;
  }

  unsigned
  speed () noexcept
  {
    return g_speed;
  }

  bool
  full_duplex () noexcept
  {
    return g_full_duplex;
  }

  std::uint32_t
  phy_id () noexcept
  {
    return g_phy_id;
  }

  unsigned
  version () noexcept
  {
    return g_version;
  }

  const char*
  address_source () noexcept
  {
    return g_address_source;
  }

  const statistics&
  stats () noexcept
  {
    return g_stats;
  }

  namespace
  {
    std::uint8_t g_loop_frame[128];
    volatile bool g_loop_seen = false;

    void
    loop_received (std::span<const std::uint8_t> frame) noexcept
    {
      if (frame.size () >= sizeof (g_loop_frame)
          && std::memcmp (frame.data (), g_loop_frame, sizeof (g_loop_frame)) == 0)
        {
          g_loop_seen = true;
        }
    }
  } // namespace

  bool
  loopback_test (bool through_phy) noexcept
  {
    if (!g_started || g_linked)
      {
        return false;
      }
    constexpr std::uint32_t CMD_PROMISC = 1u << 4;

    if (through_phy)
      {
        // 1000 Mbit/s full duplex, auto-negotiation off, loopback; a
        // Broadcom PHY loops at gigabit only as a forced master.
        mdio_write (MII_CTRL1000, 0x1800);
        mdio_write (MII_BMCR, 0x4140);
        platform::delay_us (200000);
      }
    g_speed = 1000;
    g_full_duplex = true;
    apply_link ();
    wr (UMAC_CMD, rd (UMAC_CMD) | CMD_PROMISC
                      | (through_phy ? 0u : CMD_LCL_LOOP_EN));
    platform::delay_us (10000);

    // To ourselves, from ourselves, an experimental EtherType, a pattern.
    std::memcpy (g_loop_frame, g_address.data (), 6);
    std::memcpy (g_loop_frame + 6, g_address.data (), 6);
    g_loop_frame[12] = 0x88;
    g_loop_frame[13] = 0xb5;
    for (std::size_t i = 14; i < sizeof (g_loop_frame); ++i)
      {
        g_loop_frame[i] = static_cast<std::uint8_t> (i * 7u + 3u);
      }

    const receive_handler previous = g_on_frame;
    g_on_frame = loop_received;
    g_loop_seen = false;
    g_linked = true; // lets send_ethernet() through, for this one frame
    const bool sent = send_ethernet ({ g_loop_frame, sizeof (g_loop_frame) });
    const std::uint64_t deadline = platform::time_us () + 300000u;
    while (sent && !g_loop_seen && platform::time_us () < deadline)
      {
        poll_receive ();
      }
    g_linked = false;
    g_on_frame = previous;

    // Back to normal operation: MAC off until a real link, PHY negotiating.
    wr (UMAC_CMD, rd (UMAC_CMD)
                      & ~(CMD_PROMISC | CMD_LCL_LOOP_EN | CMD_TX_EN | CMD_RX_EN));
    g_speed = 0;
    (void)phy_start ();
    return sent && g_loop_seen;
  }

  void
  renegotiate (bool gigabit) noexcept
  {
    mdio_write (MII_ADVERTISE, 0x0001 | 0x01e0 | 0x0400);
    mdio_write (MII_CTRL1000, gigabit ? 0x0200 : 0x0000);
    mdio_write (MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
  }

  void
  phy_summary (char* text, std::size_t size) noexcept
  {
    (void)mdio_read (MII_BMSR);
    std::snprintf (text, size, "BMSR %04x LPA %04x ST1000 %04x AUX %04x",
                   static_cast<unsigned> (mdio_read (MII_BMSR)),
                   static_cast<unsigned> (mdio_read (MII_LPA)),
                   static_cast<unsigned> (mdio_read (MII_STAT1000)),
                   static_cast<unsigned> (mdio_read (0x19)));
  }

  void
  print_phy () noexcept
  {
    (void)mdio_read (MII_BMSR);
    const int bmsr = mdio_read (MII_BMSR);
    kprintf ("[PHY] BMCR %04x BMSR %04x (link %s, autoneg %s) ADV %04x LPA "
             "%04x CTRL1000 %04x STAT1000 %04x AUXSTAT %04x\n",
             static_cast<unsigned> (mdio_read (MII_BMCR)),
             static_cast<unsigned> (bmsr),
             (bmsr >= 0 && (bmsr & BMSR_LSTATUS) != 0) ? "up" : "down",
             (bmsr >= 0 && (bmsr & BMSR_ANEGCOMPLETE) != 0) ? "complete"
                                                            : "not complete",
             static_cast<unsigned> (mdio_read (MII_ADVERTISE)),
             static_cast<unsigned> (mdio_read (MII_LPA)),
             static_cast<unsigned> (mdio_read (MII_CTRL1000)),
             static_cast<unsigned> (mdio_read (MII_STAT1000)),
             static_cast<unsigned> (mdio_read (0x19)));
  }
} // namespace rpi4b::genet
