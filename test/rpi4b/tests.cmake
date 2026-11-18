# -----------------------------------------------------------------------------
# What this board's tests need.
#
# Which tests exist is the directory listing beside this file -- a board builds
# what it has, so there is no capability filter and no list of names to keep in
# step with the directories. This file carries only the knobs a test cannot
# state for itself.
# -----------------------------------------------------------------------------

# Tests that reach the SD card, and so link UOS_BOARD_DEVICES
# (boards/rpi4b/board.cmake: the SD-only driver set -- the type-A ports sit
# behind the VL805 host controller; the one USB device is the DWC2 behind the
# USB-C socket, which only usb_big_test uses, below).
set (BOARD_TEST_NEED_DEVICES "")
# The board tests that use the SD card through µOS++ POSIX I/O and Chan FatFs
# (the card as /dev/sd, mounted at /sd/ by uos_test_storage_start()).
list (APPEND UOS_TESTS_STORAGE smp-shapes-test usb_big_test)

# The DWC2 USB device driver (micro-os-plus-iii-devices-bcm283x
# drivers/aarch64/src/usb_dwc2.cpp, its BCM2711 build:
# the core at USB_BASE, its interrupt INTID 105), for the one test that uses
# the USB-C socket's gadget. Not in the board's device set: no other test
# links it.
if (NOT TARGET aarch64-rpi4b-usb)
  add_library (aarch64-rpi4b-usb INTERFACE)
  target_sources (aarch64-rpi4b-usb INTERFACE
                  "${UOS_DEVICES_BCM283X_DRIVERS}/src/usb_dwc2.cpp")
endif ()

# Per-test -D flags, carried over from the per-test Makefiles.
function (board_test_defines _app _out)
  set (_d "")
  if (_app STREQUAL "cmsis-os-validator")
    # The validator is Cortex-M code. For its image only, the board
    # (boards/rpi4b/src/) maps a "no cycle counter" page where it probes the
    # DWT (mmu.cpp), and routes SGI 1, its stand-in for NVIC IRQ 0, to its
    # handler (rtos/port_isr.cpp). Every other test's image is unchanged.
    list (APPEND _d UOS_CMSIS_OS_VALIDATOR)
  elseif (_app STREQUAL "smp_mem_test" OR _app STREQUAL "smp_mem_inter_test")
    # For its image only, the board maps the RAM above the first GB page by
    # page, every page invalid until the test maps a block it allocates
    # (boards/rpi4b/src/mmu.cpp, include/mmu_hi.hpp).
    list (APPEND _d UOS_SMP_MEM_TEST)
  endif ()
  set (${_out} "${_d}" PARENT_SCOPE)
endfunction ()

# -----------------------------------------------------------------------------
# The harness suites (mutex-stress, rtos-apis, cmsis-os-validator) are no
# longer here: they are upstream's, built by the tests' builder from
# tests/sources/<suite>/ (the platform's UOS_TESTS_SUITES), with the
# validator's board glue in platforms/<P>/src/cmsis-os-validator-support.cpp.

function (board_test_libs _app _out)
  if (_app STREQUAL "usb_big_test")
    set (${_out} aarch64-rpi4b-usb PARENT_SCOPE)
  elseif (_app STREQUAL "eth-net" OR _app STREQUAL "eth-tcp-server")
    set (${_out} aarch64-rpi4b-eth PARENT_SCOPE)
  else ()
    set (${_out} "" PARENT_SCOPE)
  endif ()
endfunction ()

# -----------------------------------------------------------------------------
# The Ethernet tests: eth-net (link, DHCP, ping, log on the SD card) and
# eth-tcp-server (TCP echo server, received stream saved on the SD card).
#
# They bring the network stack with them, as the Mango Pi Wi-Fi tests do: the
# GENET driver, the lwIP glue and the shared bring-up live under
# test/boards/rpi4b/net (board support, not test code), and lwIP itself under
# test/third_party/lwip. Both need the real controller and a cable; this
# port builds a qemu image of every test, and theirs only reports SKIP.
set (_eth_tests eth-net eth-tcp-server)
# The SD card and FatFs (micro-os-plus::devices-sd).
list (APPEND UOS_TESTS_STORAGE ${_eth_tests})

# Nothing network-specific is in the repository: pass it at configure time,
#   cmake ... -D ETH_TEST_STATIC_IP=192.168.1.74/24
set (ETH_TEST_STATIC_IP "" CACHE STRING "eth tests: static a.b.c.d/len instead of DHCP")
set (ETH_TEST_GATEWAY "" CACHE STRING "eth tests: gateway to ping (default: from DHCP)")
set (ETH_TEST_PEER "" CACHE STRING "eth-net: optional second host to ping")
set (ETH_TCP_WAIT_S "60" CACHE STRING "eth-tcp-server: seconds to wait for a client")

set (_eth_net_dir "${UOS_BOARD_SRC_DIR}/net/src")
set (_eth_lwip_dir "${CMAKE_CURRENT_LIST_DIR}/../third_party/lwip/src")

if (NOT TARGET aarch64-rpi4b-eth)
  file (GLOB _eth_sources CONFIGURE_DEPENDS
        "${_eth_net_dir}/*.cpp"
        "${_eth_lwip_dir}/core/*.c"
        "${_eth_lwip_dir}/core/ipv4/*.c"
        "${_eth_lwip_dir}/netif/*.c")
  add_library (aarch64-rpi4b-eth INTERFACE)
  target_sources (aarch64-rpi4b-eth INTERFACE ${_eth_sources})
  target_include_directories (aarch64-rpi4b-eth INTERFACE
                              "${_eth_net_dir}"
                              "${_eth_lwip_dir}/include")
  target_compile_definitions (aarch64-rpi4b-eth INTERFACE
                              "ETH_TEST_STATIC_IP=\"${ETH_TEST_STATIC_IP}\""
                              "ETH_TEST_GATEWAY=\"${ETH_TEST_GATEWAY}\""
                              "ETH_TEST_PEER=\"${ETH_TEST_PEER}\""
                              "ETH_TCP_WAIT_S=${ETH_TCP_WAIT_S}")
endif ()
