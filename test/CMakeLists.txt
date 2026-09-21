# -----------------------------------------------------------------------------
# Test builds for the AArch64 port.
#
# Twelve applications x two variants = 24 targets, all from one loop. The
# application list and the per-test knobs come from the kernel repository
# (test_smpl/apps.cmake), so this file never names a test.
#
#   qemu  the build the QEMU suites run: semihosting on, no boot-card I/O
#   hwd   the build for real hardware: HW_BUILD, so the SD tests use the
#         existing FAT32 boot partition, plus the early-boot asm markers
#
# BOARD is declared by the port, above, from boards/<board>/board.cmake.
# -----------------------------------------------------------------------------

option (UOS_DEBUG_BOOT "Compile the hwd builds with the early-boot markers" OFF)

set (_boards_dir "${UOS_BOARD_SRC_DIR}")

# Every board fact this file needs was declared by boards/<BOARD>/board.cmake.
# There is deliberately no "if (BOARD STREQUAL ...)" anywhere below: a port
# with many boards cannot carry a chain per file, and a chain is exactly what
# lets a new board silently take the previous one's branch.
set (_linker        "${UOS_BOARD_LINKER_QEMU}")
set (_ncpu          "${UOS_BOARD_NCPU}")
set (_board_defines "")

# Shared by every test on this port. Semihosting is on by default, as it was
# in the Makefiles: the console reaches the emulator or the OpenOCD log, and a
# test ends its run with a semihosted exit as soon as it has a RESULT.
set (_common_defines TRACE __ARM_EABI__ SEMIHOST ${_board_defines})

# Which of the shared tests this board can build at all.
#
# Asked as a CAPABILITY question, never as a board question. A test declares
# what it needs (apps.cmake); a board declares what it has (board.cmake); a
# test is built when the board has everything it needs. So a board with no SD
# card, no USB device controller, or only one CPU needs no edit here.
set (_apps "")
foreach (_app IN LISTS UOS_TEST_APPS)
  set (_needs "")
  if ("${_app}" IN_LIST UOS_TEST_APPS_SMP_ONLY)
    list (APPEND _needs smp)
  endif ()
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_SD)
    list (APPEND _needs sdcard)
  endif ()
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_USB)
    list (APPEND _needs usb-device)
  endif ()

  set (_buildable TRUE)
  foreach (_cap IN LISTS _needs)
    if (NOT "${_cap}" IN_LIST UOS_BOARD_CAPS)
      set (_buildable FALSE)
    endif ()
  endforeach ()
  if (_buildable)
    list (APPEND _apps "${_app}")
  endif ()
endforeach ()

foreach (_app IN LISTS _apps)
  uos_test_app_defines ("${_app}" _app_defines)

  set (_libs micro-os-plus::aarch64)
  if ("${_app}" IN_LIST UOS_TEST_APPS_NEED_SD
      OR "${_app}" IN_LIST UOS_TEST_APPS_NEED_USB)
    list (APPEND _libs micro-os-plus::devices)
  endif ()

  foreach (_variant IN ITEMS qemu hwd)
    if (_variant STREQUAL "hwd")
      # DEBUG_BOOT is opt-in, as the sources themselves assume ("default
      # OFF, so the normal build stays quiet"): every early-boot marker
      # is a semihosting trap, and under a JTAG probe each one costs real
      # time. Turn it on only when bringing a board up.
      set (_variant_defines HW_BUILD)
      if (UOS_DEBUG_BOOT)
        list (APPEND _variant_defines DEBUG_BOOT)
      endif ()
    else ()
      set (_variant_defines "")
    endif ()

    uos_add_test_app (
      "${_app}-${_variant}"
      APP "${_app}"
      NCPU ${_ncpu}
      LINKER_SCRIPT "${_linker}"
      DEFINES ${_common_defines} ${_app_defines} ${_variant_defines}
      LIBRARIES ${_libs}
      OUTPUT_NAME "${_app}-${_variant}"
    )
  endforeach ()
endforeach ()

list (LENGTH _apps _n)
math (EXPR _n "${_n} * 2")
message (STATUS "µOS++ III AArch64 tests: ${_n} targets, board=${BOARD}")
