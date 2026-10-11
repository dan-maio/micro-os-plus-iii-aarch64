# micro-os-plus-iii-aarch64 — integration (xpack-development) model. The kernel is
# added by the harness and consumed as micro-os-plus::iii-core (THIN; the port
# owns startup/trace); the dissolved BCM2837 SoC is compiled locally; the
# board-fact contract comes via micro-os-plus::test-support. Exports
# micro-os-plus::aarch64. (Authored for the integration — the smp-branch root
# used the standalone sibling + devices model.)
cmake_minimum_required (VERSION 3.20)
project (micro-os-plus-iii-aarch64 LANGUAGES C CXX ASM)

# -----------------------------------------------------------------------------
# Dependencies: SMP kernel (brings iii, iii-core, port-smp-decls, test-support).
get_filename_component (_uos_siblings "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii"
     CACHE PATH "µOS++ III SMP kernel working copy")

if (NOT TARGET micro-os-plus::iii-core)
  if (NOT EXISTS "${UOS_SMP_DIR}/CMakeLists.txt")
    if (EXISTS "${_uos_siblings}/micro-os-plus-iii-smp/CMakeLists.txt")
      set (UOS_SMP_DIR "${_uos_siblings}/micro-os-plus-iii-smp" CACHE PATH
           "µOS++ III sibling working copy" FORCE)
    endif ()
  endif ()
  add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii-smp)
endif ()

file (GLOB _board_decls CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/boards/*/board.cmake")
set (_board_ids "")
foreach (_decl IN LISTS _board_decls)
  get_filename_component (_dir "${_decl}" DIRECTORY)
  get_filename_component (_id "${_dir}" NAME)
  list (APPEND _board_ids "${_id}")
endforeach ()
list (SORT _board_ids)
set (BOARD "rpi-zero-2w" CACHE STRING "Target board (one of: ${_board_ids})")
set_property (CACHE BOARD PROPERTY STRINGS ${_board_ids})
if (NOT BOARD IN_LIST _board_ids)
  message (FATAL_ERROR "BOARD=${BOARD} is not a board of this port.")
endif ()
include ("${CMAKE_CURRENT_SOURCE_DIR}/test/boards/${BOARD}/board.cmake")
foreach (_r IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_FLAGS UOS_BOARD_NCPU UOS_BOARD_CAPS
                     UOS_BOARD_LINKER_HW UOS_BOARD_DEFINES)
  if (NOT DEFINED ${_r})
    message (FATAL_ERROR "board.cmake does not set ${_r}.")
  endif ()
endforeach ()
get_filename_component (UOS_BOARD_SRC_DIR "${UOS_BOARD_SRC_DIR}" ABSOLUTE)
# The SoC a board is built on: soc/<UOS_BOARD_SOC>/ (include/ + src/). Optional
# in board.cmake -- the Pi Zero 2 W and the Pi 3 B predate it and are BCM2837.
if (NOT DEFINED UOS_BOARD_SOC)
  set (UOS_BOARD_SOC "bcm2837")
endif ()
if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/soc/${UOS_BOARD_SOC}")
  message (FATAL_ERROR "board.cmake: UOS_BOARD_SOC=${UOS_BOARD_SOC}, but there is no soc/${UOS_BOARD_SOC}/.")
endif ()
foreach (_v IN ITEMS UOS_BOARD_SRC_DIR UOS_BOARD_NCPU UOS_BOARD_CAPS
                     UOS_BOARD_LINKER_HW UOS_BOARD_LINKER_QEMU)
  set (${_v} "${${_v}}" CACHE INTERNAL "board fact")
endforeach ()
file (GLOB _board_sources CONFIGURE_DEPENDS
      "${UOS_BOARD_SRC_DIR}/src/*.cpp" "${UOS_BOARD_SRC_DIR}/src/*.S"
      "${UOS_BOARD_SRC_DIR}/src/rtos/*.cpp")
file (GLOB _soc_sources CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/soc/${UOS_BOARD_SOC}/src/*.cpp")

if (NOT TARGET micro-os-plus::aarch64)
  add_library (micro-os-plus-iii-aarch64-interface INTERFACE)
  target_include_directories (micro-os-plus-iii-aarch64-interface INTERFACE
    "include" "soc/${UOS_BOARD_SOC}/include" "${UOS_BOARD_SRC_DIR}/include")
  target_sources (micro-os-plus-iii-aarch64-interface INTERFACE
    src/context_switch.cpp src/exception_handler.cpp src/handlers.cpp src/smp_secondary.cpp
    src/rtos/os-core.cpp
    ${_soc_sources} ${_board_sources})
  target_compile_definitions (micro-os-plus-iii-aarch64-interface INTERFACE ${UOS_BOARD_DEFINES})
  target_compile_options    (micro-os-plus-iii-aarch64-interface INTERFACE ${UOS_BOARD_FLAGS})
  target_link_options       (micro-os-plus-iii-aarch64-interface INTERFACE ${UOS_BOARD_FLAGS})
  target_link_libraries (micro-os-plus-iii-aarch64-interface INTERFACE
    micro-os-plus::iii-core micro-os-plus::port-smp-decls micro-os-plus::test-support
    ${UOS_BOARD_LIBS})
  add_library (micro-os-plus::aarch64 ALIAS micro-os-plus-iii-aarch64-interface)
  set (UOS_PORT_BARE_LIB micro-os-plus::aarch64 CACHE INTERNAL "port bare library")
  message (STATUS "µOS++ IIIe aarch64 port: micro-os-plus::aarch64 (BOARD=${BOARD}, SoC=${UOS_BOARD_SOC})")
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
# Headers only: the port target above already compiles soc/bcm2837/src/*.cpp,
# so listing them here as well would link them twice.
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
# the port target compiles soc/${UOS_BOARD_SOC}/src/*.cpp for the board's SoC.
if (NOT TARGET micro-os-plus::soc-bcm2711)
  add_library (micro-os-plus-iii-soc-bcm2711-interface INTERFACE)
  target_include_directories (
    micro-os-plus-iii-soc-bcm2711-interface
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/soc/bcm2711/include"
  )
  add_library (micro-os-plus::soc-bcm2711
               ALIAS micro-os-plus-iii-soc-bcm2711-interface)
endif ()
