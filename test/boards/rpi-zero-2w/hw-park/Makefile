# Raspberry Pi Zero 2W hw-park: parking kernel for J-Link + OpenOCD debug-in-RAM.
# Builds a kernel8.img (AArch64) that does nothing but WFE, so OpenOCD always
# attaches to cores in a clean AArch64 EL2, MMU-off state.

PROJECT := hw_park
OUT_DIR := output

TOOLCHAIN_PATH := $(shell ls -d $(HOME)/.local/xPacks/@xpack-dev-tools/aarch64-none-elf-gcc/*/.content/bin 2>/dev/null | sort -V | tail -1)
ifneq ($(TOOLCHAIN_PATH),)
PREFIX := $(TOOLCHAIN_PATH)/aarch64-none-elf-
else
PREFIX := aarch64-none-elf-
endif

CC := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy

all: $(OUT_DIR)/kernel8.img

$(OUT_DIR)/kernel8.img: park.S | $(OUT_DIR)
	@$(CC) -c -o $(OUT_DIR)/park.o park.S
	@$(CC) -nostdlib -Wl,-Ttext=0x80000 -Wl,--build-id=none -o $(OUT_DIR)/park.elf $(OUT_DIR)/park.o
	@$(OBJCOPY) -O binary $(OUT_DIR)/park.elf $@
	@echo "Created: $@  (copy to the boot partition as kernel8.img)"

$(OUT_DIR):
	@mkdir -p $@

clean:
	@rm -rf $(OUT_DIR)

.PHONY: all clean
