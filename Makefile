ifndef CROSS_COMPILE
CROSS_COMPILE := $(shell \
if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' > /dev/null 2>&1; \
then echo 'riscv64-unknown-elf-'; \
elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' > /dev/null 2>&1; \
then echo 'riscv64-elf-'; \
elif riscv64-none-elf-objdump -i 2>&1 | grep 'elf64-big' > /dev/null 2>&1; \
then echo 'riscv64-none-elf-'; \
elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' > /dev/null 2>&1; \
then echo 'riscv64-linux-gnu-'; \
elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' > /dev/null 2>&1; \
then echo 'riscv64-unknown-linux-gnu-'; \
else echo "***" 1>&2; \
echo "*** Error: Cross compiler not found" 1>&2; \
echo "***" 1>&2; \
exit 1; \
fi)
endif
BUILD ?= debug
RAM ?= 128M
BUILD_DIR ?= build/$(BUILD)
ENABLE_SMP ?= 0
ifeq ($(ENABLE_SMP),1)
CPU ?= 3
else
CPU ?= 1
endif

LOG_LEVEL ?= info
LOG_COLOR_ENABLE ?= 1

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

ifeq ($(BUILD),debug)
OPT := 0
else ifeq ($(BUILD),release)
OPT := 2
else
$(error unknown BUILD value '$(BUILD)', expected debug or release)
endif

COMMON_WARNINGS := -Wall -Wextra -Werror
DISABLED_WARNINGS := -Wno-unused-parameter -Wno-unknown-attributes -Wno-main
ARCH_FLAGS := -march=rv64gc -mcmodel=medany
RUNTIME_FLAGS := -ffreestanding -fno-common -nostdlib
DEBUG_FLAGS := -ggdb -gdwarf-2 -fno-omit-frame-pointer
SAFETY_FLAGS := -fno-stack-protector -fno-pie -no-pie
INCLUDE_FLAGS := -Iinclude -Ilib/libfdt

CFLAGS := -O$(OPT)
CFLAGS += $(COMMON_WARNINGS) $(DISABLED_WARNINGS)
CFLAGS += $(ARCH_FLAGS) $(RUNTIME_FLAGS)
CFLAGS += $(DEBUG_FLAGS) $(SAFETY_FLAGS)
CFLAGS += $(INCLUDE_FLAGS)
CFLAGS += -MMD -MP
ifeq ($(ENABLE_SMP),1)
CFLAGS += -DENABLE_SMP=1
endif

ifeq ($(LOG_LEVEL),trace)
CFLAGS += -DLOG_LEVEL=LOG_LEVEL_TRACE
else ifeq ($(LOG_LEVEL),debug)
CFLAGS += -DLOG_LEVEL=LOG_LEVEL_DEBUG
else ifeq ($(LOG_LEVEL),info)
CFLAGS += -DLOG_LEVEL=LOG_LEVEL_INFO
else ifeq ($(LOG_LEVEL),warn)
CFLAGS += -DLOG_LEVEL=LOG_LEVEL_WARN
else ifeq ($(LOG_LEVEL),error)
CFLAGS += -DLOG_LEVEL=LOG_LEVEL_ERROR
else
$(error unknown LOG_LEVEL value '$(LOG_LEVEL)', expected trace, debug, info, warn, error)
endif

ifeq ($(LOG_COLOR_ENABLE),1)
CFLAGS += -DLOG_COLOR_ENABLE=1
else
CFLAGS += -DLOG_COLOR_ENABLE=0
endif

QEMU_COMMON := -machine virt -nographic
QEMU_COMMON += -m $(RAM)
QEMU_COMMON += -smp $(CPU)
QEMU_COMMON += -bios default
QEMU_COMMON += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_COMMON += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_COMMON += -global virtio-mmio.force-legacy=false

SRCS := \
boot/head.S \
boot/setup.c \
kernel/dtb.c \
kernel/main.c \
kernel/panic.c \
kernel/printk.c \
kernel/trap.c \
kernel/cpu.c \
kernel/timer.c \
kernel/irq.c \
kernel/entry.S \
kernel/process/process.c \
kernel/process/sched.c \
kernel/process/switch.S \
kernel/process/pid.c \
kernel/syscall/syscall.c \
kernel/syscall/sysfile.c \
kernel/syscall/sysproc.c \
kernel/syscall/systime.c \
kernel/console.c \
kernel/lock.c \
kernel/dev.c \
mm/init.c \
mm/mm.c \
mm/memblock.c \
mm/pgalloc.c \
mm/pgtable.c \
mm/slab.c \
mm/vmalloc.c \
mm/ioremap.c \
drivers/plic.c \
drivers/uart.c \
drivers/virtio/virtio.c \
drivers/virtio/virtio_blk.c \
fs/tmpfs/tmpfs.c \
fs/brkfs/brkfs.c \
fs/brkfs/dir.c \
fs/brkfs/file.c \
fs/brkfs/inode.c \
fs/brkfs/mount.c \
fs/brkfs/super.c \
fs/init.c \
fs/pipe.c \
fs/dcache.c \
fs/file.c \
fs/inode.c \
fs/mount.c \
fs/super.c \
fs/exec.c \
fs/path.c \
fs/filesystem.c \
lib/string.c \
lib/printf.c \
lib/qsort.c \
lib/hash.c \
lib/bitmap.c \
lib/libfdt/fdt.c \
lib/libfdt/fdt_ro.c \
lib/libfdt/fdt_wip.c \
lib/libfdt/fdt_addresses.c \
lib/libfdt/fdt_rw.c

OBJS_S := $(patsubst %.S,$(BUILD_DIR)/%.o,$(filter %.S,$(SRCS)))
OBJS_C := $(patsubst %.c,$(BUILD_DIR)/%.o,$(filter %.c,$(SRCS)))
OBJS := $(OBJS_S) $(OBJS_C)
DEPS := $(OBJS:.o=.d)
ECHO_DEFAULT_VARS := BUILD BUILD_DIR CPU RAM CROSS_COMPILE BRK_ELF ROOTFS_IMG QEMU GDB
ECHO_VARS ?= $(ECHO_DEFAULT_VARS)

.PHONY: all brk run gdb-server gdb-client rootfs clean help echo

all: brk

help:
	@echo "Targets:"
	@echo "  all / brk      Build kernel ELF"
	@echo "  run            Boot in QEMU"
	@echo "  gdb-server     Start QEMU and wait for GDB"
	@echo "  gdb-client     Connect GDB to :1234"
	@echo "  rootfs         Create rootfs image"
	@echo "  clean          Remove build outputs"
	@echo "  echo           Print selected build variables"
	@echo ""
	@echo "Configurable vars: BUILD={debug|release}, BUILD_DIR=<path>, CPU=<n>, RAM=<size>, CROSS_COMPILE=<prefix> ENABLE_SMP={0|1}"
	@echo "Echo vars: make echo [ECHO_VARS=\"VAR1 VAR2 ...\"]"

echo:
	@$(foreach var,$(ECHO_VARS),echo "$(var): $($(var))";)

brk: $(BRK_ELF)

run: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -kernel $(BRK_ELF)

gdb-server: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -S -s -kernel $(BRK_ELF)

gdb-client: $(BRK_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(BRK_ELF)

rootfs: $(ROOTFS_IMG)

$(BRK_LD): $(BRK_LD_S)
	@mkdir -p $(dir $@)
	$(CPP) $(CFLAGS) -o $@ $<

$(BRK_ELF): $(OBJS) $(BRK_LD)
	$(LD) -z max-page-size=4096 -T $(BRK_LD) -static -o $@ $(OBJS)

$(ROOTFS_IMG):
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1M count=64 status=progress
	mkfs.ext4 -F -L BRK_ROOT -m 0 $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)
