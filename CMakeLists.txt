# -----------------------------------------------------------------------------
# micro-os-plus-iii-aarch64 -- ARMv8-A (64-bit, EL1) SMP port of µOS++ III.
#
# An independent project. It consumes the kernel and the device drivers;
# neither of them knows this project exists, so the dependency runs one way
# and the kernel tree stays merge-clean against upstream.
#
#   external/micro-os-plus-iii-smp      the kernel and the shared tests
#   external/micro-os-plus-iii-devices  SD, flatfs, DWC2, FatFs, BCM2837 SoC
#
# Boards are build configurations of this one port, not separate ports:
# Pi Zero 2 W and Pi 3 B differ by a linker script and a banner macro.
#
# Exports: micro-os-plus::aarch64
# -----------------------------------------------------------------------------
cmake_minimum_required (VERSION 3.20)

project (micro-os-plus-iii-aarch64 LANGUAGES C CXX ASM)

set (UOS_AARCH64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# -----------------------------------------------------------------------------
# Dependencies. Submodules by default; point the cache variables elsewhere to
# build against a working copy.
set (UOS_SMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/micro-os-plus-iii-smp"
     CACHE PATH "µOS++ III SMP kernel")
set (UOS_DEVICES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/micro-os-plus-iii-devices"
     CACHE PATH "µOS++ III device drivers")

foreach (_dep IN ITEMS "${UOS_SMP_DIR}" "${UOS_DEVICES_DIR}")
  if (NOT EXISTS "${_dep}/CMakeLists.txt")
    message (FATAL_ERROR
      "Missing dependency at ${_dep}.\n"
      "Run: git submodule update --init --recursive")
  endif ()
endforeach ()

add_subdirectory ("${UOS_SMP_DIR}" micro-os-plus-iii-smp)
add_subdirectory ("${UOS_DEVICES_DIR}" micro-os-plus-iii-devices)

# -----------------------------------------------------------------------------
# The port itself.
add_library (micro-os-plus-iii-aarch64-interface INTERFACE)

target_include_directories (
  micro-os-plus-iii-aarch64-interface
  INTERFACE "include"
)

target_sources (
  micro-os-plus-iii-aarch64-interface
  INTERFACE
        src/startup.S
        src/context_switch.cpp
        src/exception_handler.cpp
        src/handlers.cpp
        src/mmu.cpp
        src/port_sys.cpp
        src/smp.cpp
        src/smp_secondary.cpp
        src/timer_arm.cpp
        # The port's half of the scheduler: bring-up, the kernel lock and the
        # context-switch hooks. The kernel's own src/rtos/os-core.cpp is the
        # portable half and is compiled alongside it, exactly as the previous
        # Makefiles did.
        src/rtos/os-core.cpp
)

target_compile_options (
  micro-os-plus-iii-aarch64-interface
  # Only the silicon. The flags every bare-metal application shares
  # (-O2, -ffunction-sections, -fno-exceptions, -nostartfiles, ...) are
  # set once by uos_add_app.
  INTERFACE -mcpu=cortex-a53
)

target_link_libraries (
  micro-os-plus-iii-aarch64-interface
  INTERFACE
        micro-os-plus::iii
        # os-decls.h: the C++ half of the SMP port contract, identical to the
        # AArch32 port's. This project supplies os-c-decls.h and os-inlines.h,
        # which are where the word size and the instructions actually live.
        micro-os-plus::port-smp-decls
        micro-os-plus::soc-bcm2837
)

add_library (micro-os-plus::aarch64 ALIAS micro-os-plus-iii-aarch64-interface)

# -----------------------------------------------------------------------------
if (PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
  add_subdirectory (test)
endif ()

message (STATUS "µOS++ III AArch64 port: micro-os-plus::aarch64")
