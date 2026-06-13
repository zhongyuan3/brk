BRK_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BRK_TOP_MAKEFILE := $(BRK_ROOT)/Makefile

MAKEFLAGS += --no-print-directory --silent

BRK_BUILD_ROOT ?= build

ARCH ?= riscv

ifeq ($(ARCH),riscv)
include $(BRK_ROOT)/arch/$(ARCH)/config.mk
else
$(error ARCH=$(ARCH) is not supported)
endif

BUILD ?= debug
RAM ?= 128M
ENABLE_SMP ?= 1
ifeq ($(ENABLE_SMP),1)
CPU ?= 3
BUILD_DIR ?= $(BRK_BUILD_ROOT)/$(BUILD)-smp
else
CPU ?= 1
BUILD_DIR ?= $(BRK_BUILD_ROOT)/$(BUILD)
endif

CC := $(CROSS_COMPILE)gcc
CPP := $(CC) -E
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

BRK_WARN_FLAGS := -Wall -Wextra -Werror
BRK_FREESTANDING_FLAGS := -ffreestanding -fno-common -nostdlib \
	-fno-stack-protector -fno-pie -no-pie

CFLAGS := $(BRK_WARN_FLAGS) $(BRK_ARCH_FLAGS) $(BRK_FREESTANDING_FLAGS)
LDFLAGS := -z max-page-size=4096

ifeq ($(ENABLE_SMP),1)
CFLAGS += -DENABLE_SMP=1
endif

ifeq ($(BUILD),debug)
ENABLE_GC ?= 0
CFLAGS += -O0 -ggdb -gdwarf-2 -fno-omit-frame-pointer
ifeq ($(ENABLE_GC),1)
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += --gc-sections
endif
else ifeq ($(BUILD),release)
ifeq ($(ENABLE_GC),0)
$(error ENABLE_GC=0 is not supported for release builds)
endif
CFLAGS += -O2 -DNDEBUG -fomit-frame-pointer -fmerge-all-constants \
	-fno-semantic-interposition -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
LDFLAGS += --gc-sections
else
$(error unknown BUILD value '$(BUILD)', expected debug or release)
endif

CFLAGS += -MMD -MP
CFLAGS += -I$(BRK_ROOT)/arch/$(ARCH)/include
CFLAGS += -I$(BRK_ROOT)/include -I$(BRK_ROOT)/vendor/libfdt

export BRK_ROOT BRK_TOP_MAKEFILE CROSS_COMPILE CC LD AS AR CFLAGS LDFLAGS

BRK_LINKER_SCRIPT_SRC := arch/$(ARCH)/kernel/brk.ld.S
BRK_LINKER_SCRIPT := $(BUILD_DIR)/arch/$(ARCH)/kernel/brk.ld
BRK_ELF := $(BUILD_DIR)/brk.elf
BRK_ROOTFS_IMG := $(BRK_BUILD_ROOT)/rootfs.img
BRK_DISASM := $(BUILD_DIR)/brk.txt

# Each top-level module produces $(BUILD_DIR)/<name>/built-in.o
core-y := arch/$(ARCH)
core-y += kernel
core-y += mm
core-y += drivers
core-y += fs
core-y += lib
core-y += vendor

BRK_BUILTIN_OBJS := $(addprefix $(BUILD_DIR)/,$(patsubst %,%/built-in.o,$(core-y)))

QEMU_OPTS += -m $(RAM)
QEMU_OPTS += -smp $(CPU)
QEMU_OPTS += -bios default
QEMU_OPTS += -drive file=$(BRK_ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_OPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_OPTS += -global virtio-mmio.force-legacy=false

ifdef build
src := $(build)
obj := $(BUILD_DIR)/$(build)
include scripts/Makefile.build
else

.PHONY: all brk run gdb-server gdb-client rootfs clean clean-kernel \
	help dts objdump FORCE

all: brk

help:
	@echo "Targets:"
	@echo "  all / brk              Build uniprocessor kernel"
	@echo "  run                    Boot uniprocessor kernel in QEMU"
	@echo "  gdb-server             QEMU + GDB stub (uniprocessor)"
	@echo "  gdb-client             Connect GDB to :1234"
	@echo "  rootfs                 Create brkfs root disk"
	@echo "  clean                  Remove all build outputs"
	@echo "  clean-kernel           Remove current variant build directory"
	@echo "  dts                    Dump QEMU virt device tree as DTS"
	@echo "  objdump                Disassemble kernel ELF"
	@echo "Configurable vars:"
	@echo "  ARCH=<name>"
	@echo "  CROSS_COMPILE=<prefix>"
	@echo "  BUILD={debug|release}  (default: debug)"
	@echo "  BUILD_DIR=<path>       (default: build/<BUILD> or build/<BUILD>-smp)"
	@echo "  BRK_BUILD_ROOT=<path>  (default: build)"
	@echo "  CPU=<n>                (default 1, or 3 when ENABLE_SMP=1)"
	@echo "  RAM=<size>             (default 128M)"
	@echo "  ENABLE_SMP={0|1}       (default 1)"
	@echo "  ENABLE_GC={0|1}        (default 0, only for debug builds)"

brk: $(BRK_ELF)

$(BRK_LINKER_SCRIPT): $(BRK_ROOT)/$(BRK_LINKER_SCRIPT_SRC)
	@mkdir -p $(dir $@)
	@echo "  CPP     $@"
	@$(CPP) $(CFLAGS) -o $@ $<

$(BRK_ELF): $(BRK_BUILTIN_OBJS) $(BRK_LINKER_SCRIPT)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -T $(BRK_LINKER_SCRIPT) -static -o $@ $(BRK_BUILTIN_OBJS)

$(BUILD_DIR)/%/built-in.o: FORCE
	@$(MAKE) -f $(BRK_TOP_MAKEFILE) build=$*

run: $(BRK_ELF) $(BRK_ROOTFS_IMG)
	$(QEMU) $(QEMU_OPTS) -kernel $(BRK_ELF)

gdb-server: $(BRK_ELF) $(BRK_ROOTFS_IMG)
	$(QEMU) $(QEMU_OPTS) -S -s -kernel $(BRK_ELF)

gdb-client: $(BRK_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(BRK_ELF)

rootfs: $(BRK_ROOTFS_IMG)

$(BRK_ROOTFS_IMG): $(BRK_ROOT)/scripts/mkrootfs.sh
	@./scripts/mkrootfs.sh "$(BRK_ROOTFS_IMG)"

clean:
	$(RM) -r $(BRK_BUILD_ROOT)

clean-kernel:
	$(RM) -r $(BUILD_DIR)

dts: $(BUILD_DIR)/devicetree.dts

objdump: $(BRK_DISASM)

$(BRK_DISASM): $(BRK_ELF)
	@$(OBJDUMP) -S $< > $@

$(BUILD_DIR)/devicetree.dts: $(BUILD_DIR)/devicetree.dtb
	@mkdir -p $(dir $@)
	dtc -I dtb -O dts -o $@ $<

$(BUILD_DIR)/devicetree.dtb:
	@mkdir -p $(dir $@)
	$(QEMU) -machine virt,dumpdtb=$@ -nographic

endif
