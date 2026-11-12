// genet.hpp - the Raspberry Pi 4 B Ethernet: Broadcom GENET v5 MAC at
// 0xFD580000 with a BCM54213PE gigabit PHY on RGMII (MDIO address 1).
//
// A polled driver, no interrupts: one thread owns it and calls poll().
//
//   start()            reset the MAC, set the address, set up the two DMA
//                      rings, start auto-negotiation
//   poll()             hand received frames to the receive handler; follow
//                      the PHY link and report changes to the link handler
//   send_ethernet()    transmit one frame (the MAC appends the FCS)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rpi4b::genet
{
  using mac_address = std::array<std::uint8_t, 6>;
  using receive_handler = void (*) (std::span<const std::uint8_t> frame) noexcept;
  using link_handler = void (*) (bool up) noexcept;

  struct statistics
  {
    std::uint32_t rx_frames{ 0 };
    std::uint32_t rx_errors{ 0 };
    std::uint32_t tx_frames{ 0 };
    std::uint32_t tx_errors{ 0 };
    std::uint32_t link_changes{ 0 };
  };

  /// Bring the controller up. False when no GENET v5 answers at the base
  /// address or the PHY does not respond.
  [[nodiscard]] bool start () noexcept;

  /// Service the receive ring and the link. Call often (every 1-2 ms).
  void poll () noexcept;

  /// Send one Ethernet frame (destination, source, type, payload; no FCS).
  [[nodiscard]] bool send_ethernet (std::span<const std::uint8_t> frame) noexcept;

  void set_receive_handler (receive_handler handler) noexcept;
  void set_link_handler (link_handler handler) noexcept;

  [[nodiscard]] const mac_address& address () noexcept;
  [[nodiscard]] bool linked () noexcept;
  /// Negotiated speed in Mbit/s (10, 100, 1000); 0 while the link is down.
  [[nodiscard]] unsigned speed () noexcept;
  [[nodiscard]] bool full_duplex () noexcept;
  /// PHY identifier, (PHYSID1 << 16) | PHYSID2.
  [[nodiscard]] std::uint32_t phy_id () noexcept;
  /// GENET major version read from SYS_REV_CTRL (5 on the BCM2711).
  [[nodiscard]] unsigned version () noexcept;
  /// Where the MAC address came from: "firmware", "controller" or "local".
  [[nodiscard]] const char* address_source () noexcept;
  [[nodiscard]] const statistics& stats () noexcept;

  /// Send one frame to ourselves and expect it back, without a cable.
  /// `through_phy` loops in the PHY (the frame crosses RGMII both ways);
  /// otherwise in the MAC. Usable only while the link is down; restarts
  /// auto-negotiation afterwards.
  [[nodiscard]] bool loopback_test (bool through_phy) noexcept;

  /// Restart auto-negotiation, with or without 1000BASE-T in the offer. A
  /// link partner or cable that cannot settle a gigabit negotiation often
  /// links at once when only 10/100 is offered.
  void renegotiate (bool gigabit) noexcept;

  /// The PHY's state as short text ("BMSR 7949 LPA 0000 ST1000 0000 AUX
  /// 3000"), for a result line when the console is not being read.
  void phy_summary (char* text, std::size_t size) noexcept;

  /// Print the PHY's basic registers: the evidence when a link stays down.
  void print_phy () noexcept;
} // namespace rpi4b::genet
