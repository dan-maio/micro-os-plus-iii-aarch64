# micro-os-plus-iii-aarch64 — integration (xpack-development) model. The kernel is
# added by the harness and consumed as micro-os-plus::iii-core (THIN; the port
# owns startup/trace); the dissolved SoC targets are defined here (their
# sources are compiled through the consumer's micro-os-plus::board); the
# board-fact contract comes via micro-os-plus::test-support. Exports
# micro-os-plus::aarch64. (Authored for the integration — the smp-branch root
# used the standalone sibling + devices model.)
cmake_minimum_required (VERSION 3.20)
project (micro-os-plus-iii-aarch64 LANGUAGES C CXX ASM)

# -----------------------------------------------------------------------------
# The kernel (micro-os-plus::iii and its parts) is not added here. Like every
# µOS++ port, this is a leaf library: the consumer (the tests, an application)
# adds the kernel. The targets below link kernel targets by name, which CMake
# resolves when it generates, so the kernel may be added after the port.

set (UOS_AARCH64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# The port reads no board. A board (its SoC folder, flags, defines, start-up and
# linker scripts) is the consumer's: the test platforms of micro-os-plus-iii
# build a `micro-os-plus::board` target and link it after this one.
if (NOT TARGET micro-os-plus::aarch64)
  add_library (micro-os-plus-iii-aarch64-interface INTERFACE)
  target_include_directories (micro-os-plus-iii-aarch64-interface INTERFACE "include")
  target_sources (micro-os-plus-iii-aarch64-interface INTERFACE
    src/context_switch.cpp src/exception_handler.cpp src/handlers.cpp src/smp_secondary.cpp
    src/rtos/os-core.cpp)
  target_link_libraries (micro-os-plus-iii-aarch64-interface INTERFACE
    micro-os-plus::iii-core micro-os-plus::port-smp-decls micro-os-plus::test-support)
  add_library (micro-os-plus::aarch64 ALIAS micro-os-plus-iii-aarch64-interface)
  message (STATUS "µOS++ IIIe aarch64 port: micro-os-plus::aarch64")
endif ()

# -----------------------------------------------------------------------------
# Dissolved devices support: SD host, USB, and FatFs stack.
if (NOT TARGET micro-os-plus::devices)
  add_library (micro-os-plus-iii-devices-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs"
  )
  target_sources (
    micro-os-plus-iii-devices-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/usb_dwc2.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/ff.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/diskio_sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/fatfs_hw.cpp"
  )
  add_library (micro-os-plus::devices ALIAS micro-os-plus-iii-devices-interface)
endif ()

# The SD-card half of the set above, without the DWC2 USB driver. A board with
# no USB device controller the tests can use (the Pi 4 B: its USB-C port is
# DWC2 in a different place, and its type-A ports sit behind the VL805) links
# this one instead -- usb_dwc2.cpp includes the BCM2837 headers and would not
# compile for another SoC.
if (NOT TARGET micro-os-plus::devices-sd)
  add_library (micro-os-plus-iii-devices-sd-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-devices-sd-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs"
  )
  target_sources (
    micro-os-plus-iii-devices-sd-interface
    INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/src/flatfs.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/ff.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/diskio_sd.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/drivers/fatfs/fatfs_hw.cpp"
  )
  add_library (micro-os-plus::devices-sd ALIAS micro-os-plus-iii-devices-sd-interface)
endif ()

# -----------------------------------------------------------------------------
# Dissolved SoC support: BCM2837 silicon (formerly micro-os-plus-iii-devices).
# Headers only: the board target (micro-os-plus::board, built by the consumer)
# compiles soc/<soc>/src/*.cpp, so listing them here as well would link them
# twice.
if (NOT TARGET micro-os-plus::soc-bcm2837)
  add_library (micro-os-plus-iii-soc-bcm2837-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-soc-bcm2837-interface
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/soc/bcm2837/include"
  )
  add_library (micro-os-plus::soc-bcm2837
               ALIAS micro-os-plus-iii-soc-bcm2837-interface)
endif ()

# BCM2711 silicon (Raspberry Pi 4 B): headers only, like the BCM2837 one --
# the board target compiles soc/bcm2711/src/*.cpp.
if (NOT TARGET micro-os-plus::soc-bcm2711)
  add_library (micro-os-plus-iii-soc-bcm2711-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-soc-bcm2711-interface
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/soc/bcm2711/include"
  )
  add_library (micro-os-plus::soc-bcm2711
               ALIAS micro-os-plus-iii-soc-bcm2711-interface)
endif ()
