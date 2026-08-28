BUILD_ROOT ?= build

MAKEFLAGS += --no-print-directory --silent

ARCH ?= riscv

CFLAGS :=

ifeq ($(ARCH),riscv)
include src/arch/$(ARCH)/config.mk
else
$(error Unknown ARCH value '$(ARCH)')
endif

BUILD ?= debug
RAM ?= 128M
ENABLE_SMP ?= 1
ifeq ($(ENABLE_SMP),1)
CPU ?= 3
KERNEL_BUILD_ROOT ?= $(BUILD_ROOT)/$(BUILD)-smp
CFLAGS += -DENABLE_SMP=1
else
CPU := 1
KERNEL_BUILD_ROOT ?= $(BUILD_ROOT)/$(BUILD)
endif

CC := $(CROSS_COMPILE)gcc
CPP := $(CC) -E
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Werror
CFLAGS += -ffreestanding
CFLAGS += -fno-common
CFLAGS += -nostdlib
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pie
CFLAGS += -no-pie

ifeq ($(BUILD),debug)
CFLAGS += -O0
CFLAGS += -ggdb
CFLAGS += -gdwarf-2
CFLAGS += -fno-omit-frame-pointer
else ifeq ($(BUILD),release)
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
$(error unknown BUILD value '$(BUILD)')
endif

CFLAGS += -MMD
CFLAGS += -MP
CFLAGS += -Isrc/arch/$(ARCH)/include
CFLAGS += -Isrc/include
CFLAGS += -Ivendor/libfdt

LDFLAGS := -z max-page-size=4096

export ARCH
export BUILD_ROOT KERNEL_BUILD_ROOT
export CROSS_COMPILE CC LD AS AR CFLAGS LDFLAGS

LINKER_SCRIPT_SRC := src/arch/$(ARCH)/kernel/brk.ld.S
LINKER_SCRIPT := $(KERNEL_BUILD_ROOT)/brk.ld
KERNEL_ELF := $(KERNEL_BUILD_ROOT)/brk.elf
ROOTFS_IMG := $(BUILD_ROOT)/rootfs.img

core-y := src
core-y += vendor

BUILT_IN_OBJS := $(addprefix $(KERNEL_BUILD_ROOT)/,$(patsubst %,%/built-in.o,$(core-y)))

QEMU_OPTS += -m $(RAM)
QEMU_OPTS += -smp $(CPU)
QEMU_OPTS += -bios default
QEMU_OPTS += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_OPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_OPTS += -global virtio-mmio.force-legacy=false

.PHONY: all brk run gdb-server gdb-client rootfs clean clean-kernel help FORCE

all: brk

brk: $(KERNEL_ELF)

help:
	@echo "Targets:"
	@echo "  all / brk              Build uniprocessor kernel"
	@echo "  run                    Boot uniprocessor kernel in QEMU"
	@echo "  gdb-server             QEMU + GDB stub (uniprocessor)"
	@echo "  gdb-client             Connect GDB to :1234"
	@echo "  rootfs                 Create brkfs root disk"
	@echo "  clean                  Remove all build outputs"
	@echo "  clean-kernel           Remove current variant build directory"
	@echo "Configurable vars:"
	@echo "  ARCH=<name>"
	@echo "  CROSS_COMPILE=<prefix>"
	@echo "  BUILD={debug|release}  (default: debug)"
	@echo "  BUILD_ROOT=<path>      (default: build)"
	@echo "  ENABLE_SMP={0|1}       (default 1)"
	@echo "  CPU=<n>                (default 1, or 3 when ENABLE_SMP=1)"
	@echo "  RAM=<size>             (default 128M)"

$(LINKER_SCRIPT): $(LINKER_SCRIPT_SRC)
	@mkdir -p $(dir $@)
	@echo "  CPP     $@"
	@$(CPP) $(CFLAGS) -o $@ $<

$(KERNEL_ELF): $(BUILT_IN_OBJS) $(LINKER_SCRIPT)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -static -o $@ $(BUILT_IN_OBJS)

$(KERNEL_BUILD_ROOT)/%/built-in.o: FORCE
	@$(MAKE) -f scripts/Makefile.build build=$*

run: $(KERNEL_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_OPTS) -kernel $(KERNEL_ELF)

gdb-server: $(KERNEL_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_OPTS) -S -s -kernel $(KERNEL_ELF)

gdb-client: $(KERNEL_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(KERNEL_ELF)

rootfs: $(ROOTFS_IMG)

$(ROOTFS_IMG): scripts/mkrootfs.sh
	@./scripts/mkrootfs.sh "$(ROOTFS_IMG)"

clean:
	$(RM) -r $(BUILD_ROOT)

clean-kernel:
	$(RM) -r $(KERNEL_BUILD_ROOT)
