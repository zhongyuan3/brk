BRK_SRC := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BRK_TOP_MAKEFILE := $(BRK_SRC)/Makefile

# Suppress recursive-make noise: directory banners and "is up to date" messages.
# @echo in recipes (CC/LD/AS) still prints when something is actually built.
MAKEFLAGS += --no-print-directory --silent

include $(BRK_SRC)/scripts/toolchain.mk

BUILD ?= debug
RAM ?= 128M
BUILD_DIR ?= build/$(BUILD)
ENABLE_SMP ?= 0
ifeq ($(ENABLE_SMP),1)
CPU ?= 3
else
CPU ?= 1
endif

CC := $(CROSS_COMPILE)gcc
CPP := $(CC) -E
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
GDB := gdb-multiarch
QEMU := qemu-system-riscv64

BRK_LD_S := kernel/brk.ld.S
BRK_LD := $(BUILD_DIR)/kernel/brk.ld
BRK_ELF := $(BUILD_DIR)/brk.elf
ROOTFS_IMG := $(BUILD_DIR)/rootfs.img

CFLAGS := -Wall
CFLAGS += -Wextra
CFLAGS += -Werror
CFLAGS += -march=rv64gc
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common
CFLAGS += -nostdlib
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pie
CFLAGS += -no-pie

LDFLAGS := -z max-page-size=4096

ifeq ($(ENABLE_SMP),1)
CFLAGS += -DENABLE_SMP=1
endif

ifeq ($(BUILD),debug)
ENABLE_GC ?= 0
CFLAGS += -O0
CFLAGS += -ggdb
CFLAGS += -gdwarf-2
CFLAGS += -fno-omit-frame-pointer
ifeq ($(ENABLE_GC),1)
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
LDFLAGS += --gc-sections
endif
else ifeq ($(BUILD),release)
ifeq ($(ENABLE_GC),0)
$(error ENABLE_GC=0 is not supported for release builds)
endif
CFLAGS += -O2
CFLAGS += -DNDEBUG
CFLAGS += -fomit-frame-pointer
CFLAGS += -fmerge-all-constants
CFLAGS += -fno-semantic-interposition
CFLAGS += -fno-unwind-tables
CFLAGS += -fno-asynchronous-unwind-tables
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
LDFLAGS += --gc-sections
else
$(error unknown BUILD value '$(BUILD)', expected debug or release)
endif

CFLAGS += -MMD -MP

CFLAGS += -I$(BRK_SRC)/include
CFLAGS += -I$(BRK_SRC)/vendor/libfdt

export BRK_SRC BRK_TOP_MAKEFILE BUILD_DIR CROSS_COMPILE CC LD AS AR CFLAGS LDFLAGS

QEMU_COMMON := -machine virt -nographic
QEMU_COMMON += -m $(RAM)
QEMU_COMMON += -smp $(CPU)
QEMU_COMMON += -bios default
QEMU_COMMON += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_COMMON += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_COMMON += -global virtio-mmio.force-legacy=false

# Top-level modules: each builds $(BUILD_DIR)/<name>/built-in.o
core-y := boot
core-y += kernel
core-y += mm
core-y += drivers
core-y += fs
core-y += lib
core-y += vendor

core-builtin := $(addprefix $(BUILD_DIR)/,$(patsubst %,%/built-in.o,$(core-y)))

ifdef build
# Subdirectory build (invoked recursively)
src := $(build)
obj := $(BUILD_DIR)/$(build)
include scripts/Makefile.build
else
# Top-level build

.PHONY: all brk run gdb-server gdb-client rootfs clean help FORCE

all: brk

help:
	@echo "Targets:"
	@echo "  all / brk      Build kernel ELF"
	@echo "  run            Boot in QEMU"
	@echo "  gdb-server     Start QEMU and wait for GDB"
	@echo "  gdb-client     Connect GDB to :1234"
	@echo "  rootfs         Create brkfs root disk (scripts/mkrootfs.sh + sibling brk-user / brkfstools)"
	@echo "  clean          Remove build outputs"
	@echo "Configurable vars:"
	@echo "  BUILD={debug|release}"
	@echo "  BUILD_DIR=<path>"
	@echo "  CPU=<n>"
	@echo "  RAM=<size>"
	@echo "  CROSS_COMPILE=<prefix>"
	@echo "  ENABLE_SMP={0|1}"
	@echo "  ENABLE_GC={0|1}  (debug only, default 0; release always uses GC)"

brk: $(BRK_ELF)

run: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -kernel $(BRK_ELF)

gdb-server: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -S -s -kernel $(BRK_ELF)

gdb-client: $(BRK_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(BRK_ELF)

rootfs: $(ROOTFS_IMG)

$(BRK_LD): $(BRK_SRC)/$(BRK_LD_S)
	@mkdir -p $(dir $@)
	@echo "  CPP     $@"
	@$(CPP) $(CFLAGS) -o $@ $<

$(BRK_ELF): $(core-builtin) $(BRK_LD)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -T $(BRK_LD) -static -o $@ $(core-builtin)

# Build each top-level module's built-in.o via recursive make
$(BUILD_DIR)/%/built-in.o: FORCE
	@$(MAKE) -f $(BRK_TOP_MAKEFILE) build=$*

$(ROOTFS_IMG): $(BRK_SRC)/scripts/mkrootfs.sh
	@./scripts/mkrootfs.sh "$(ROOTFS_IMG)"

clean:
	$(RM) -r $(BUILD_DIR)

.PHONY: dts

dts: $(BUILD_DIR)/devicetree.dts

$(BUILD_DIR)/devicetree.dts: $(BUILD_DIR)/devicetree.dtb
	@mkdir -p $(dir $@)
	dtc -I dtb -O dts -o $@ $<

$(BUILD_DIR)/devicetree.dtb:
	@mkdir -p $(dir $@)
	$(QEMU) -machine virt,dumpdtb=$@ -nographic

endif
