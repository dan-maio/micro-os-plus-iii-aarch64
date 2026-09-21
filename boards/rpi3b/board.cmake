# -----------------------------------------------------------------------------
# Raspberry Pi 3 B — BCM2837, 4x Cortex-A53, ARMv8-A.
#
# Same silicon and same sources as rpi-zero-2w; a different linker script, and
# an ACT LED that is not a SoC GPIO at all but VideoCore expander GPIO 130,
# driven through the mailbox -- so no LED_PIN.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../rpi-zero-2w")

set (UOS_BOARD_FLAGS -mcpu=cortex-a53)
set (UOS_BOARD_NCPU  4)

# What this board can do. The test matrix asks about capabilities; it never
# asks which board it is building for.
set (UOS_BOARD_CAPS  smp sdcard usb-device led)

set (UOS_BOARD_LIBS  micro-os-plus::soc-bcm2837)

set (UOS_BOARD_LINKER_HW   "${UOS_BOARD_SRC_DIR}/linker-rpi3b.ld")
set (UOS_BOARD_LINKER_QEMU "${UOS_BOARD_SRC_DIR}/linker-rpi3b.ld")

set (UOS_BOARD_DEFINES
     SOC_BCM2837 BOARD_RPI3B
     SEMIHOST_TRAP_CHOSEN SEMIHOST_TRAP_HLT
     OS_SMP_IPI_SGI=0
     "PORT_GREETING=\"µOS++ Raspberry Pi 3 B (BCM2837, 4x Cortex-A53)\"")
