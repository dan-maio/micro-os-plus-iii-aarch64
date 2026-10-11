# -----------------------------------------------------------------------------
# Raspberry Pi 4 Model B — BCM2711, 4x Cortex-A72, ARMv8-A.
#
# Its own sources (src/, include/) and its own SoC (soc/bcm2711/): what makes
# it a different board from the Pi 3 family is not a define but the silicon --
#
#   peripherals      0xFE000000 (the Pi 3's 0x3F000000), soc/bcm2711/
#   interrupts       GIC-400: generic-timer PPI 30 + SGI 0 as the reschedule
#                    IPI (src/port_sys.cpp, src/rtos/port_isr.cpp), not the
#                    BCM2837's per-core mailboxes and timer routing
#   MMU              4 GB VA (T0SZ=32): the devices sit at 0xFC000000+, and
#                    the VideoCore memory at the top of the first GB stays
#                    unmapped (src/mmu.cpp)
#   RAM              0x80000 .. 0x3B3FFFFF, below gpu_mem (linker.ld,
#                    include/board_ram.h)
#   SD card          EMMC2 @ 0xFE340000 on the board; QEMU raspi4b attaches
#                    its -drive if=sd card to the Arasan SDHCI @ 0xFE300000
#   LED              ACT = GPIO 42, active high
#   boot             EL3 is handled as well as EL2 (src/startup.S)
#
# No USB device test: the USB-C port's DWC2 is not where the BCM2837's is and
# the type-A ports are behind the VL805 host, so this board links the SD-only
# driver set (micro-os-plus::devices-sd) and has no test/rpi4b/usb_test.
# -----------------------------------------------------------------------------

set (UOS_BOARD_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}")
set (UOS_BOARD_SOC     bcm2711)

# -Os overrides the -O2 that uos-app.cmake gives every bare-metal target
# (interface options come after the target's own): the image is loaded over
# a ~0.6 KB/s CMSIS-DAP JTAG link, so every KB is ~1.7 s per hardware test.
set (UOS_BOARD_FLAGS -mcpu=cortex-a72 -Os)
set (UOS_BOARD_NCPU  4)

# What this board can do. The test matrix asks about capabilities; it never
# asks which board it is building for.
set (UOS_BOARD_CAPS  smp sdcard led)

set (UOS_BOARD_LIBS  micro-os-plus::soc-bcm2711)

# The driver set an application links when it needs the SD card.
set (UOS_BOARD_DEVICES  micro-os-plus::devices-sd)

set (UOS_BOARD_LINKER_HW   "${UOS_BOARD_SRC_DIR}/linker.ld")
set (UOS_BOARD_LINKER_QEMU "${UOS_BOARD_SRC_DIR}/linker.ld")

# The onboard green ACT LED: GPIO 42, active high.
set (LED_PIN 42 CACHE STRING "GPIO driven as the user LED")

# The SD host the tests use (drivers/src/sd.cpp): EMMC2 on the board, the
# Arasan SDHCI under QEMU, which is where raspi4b plugs the -drive if=sd card.
set (UOS_BOARD_DEFINES
     SOC_BCM2711
     SEMIHOST_TRAP_CHOSEN SEMIHOST_TRAP_HLT
     OS_SMP_IPI_SGI=0
     LED_PIN=${LED_PIN}
     SD_EMMC_BASE=0xFE340000u
     SD_EMMC_BASE_QEMU=0xFE300000u
     "PORT_GREETING=\"µOS++ Raspberry Pi 4 B (BCM2711, 4x Cortex-A72)\"")
