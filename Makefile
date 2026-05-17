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
CFLAGS += -ggdb
CFLAGS += -gdwarf-2
CFLAGS += -fno-omit-frame-pointer
CFLAGS += -march=rv64gc
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common
CFLAGS += -nostdlib
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pie
CFLAGS += -no-pie

ifeq ($(ENABLE_SMP),1)
CFLAGS += -DENABLE_SMP=1
endif

ifeq ($(BUILD),debug)
CFLAGS += -O0
else ifeq ($(BUILD),release)
CFLAGS += -O2
CFLAGS += -DNDEBUG
else
$(error unknown BUILD value '$(BUILD)', expected debug or release)
endif

CFLAGS += -MMD -MP

CFLAGS += -Iinclude
CFLAGS += -Ivendor/libfdt

QEMU_COMMON := -machine virt -nographic
QEMU_COMMON += -m $(RAM)
QEMU_COMMON += -smp $(CPU)
QEMU_COMMON += -bios default
QEMU_COMMON += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_COMMON += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_COMMON += -global virtio-mmio.force-legacy=false

SRCS := boot/head.S
SRCS += boot/setup.c
SRCS += kernel/dtb.c
SRCS += kernel/ktime.c
SRCS += kernel/timekeeper.c
SRCS += kernel/main.c
SRCS += kernel/panic.c
SRCS += kernel/printk.c
SRCS += kernel/trap.c
SRCS += kernel/cpu.c
SRCS += kernel/timer.c
SRCS += kernel/irq.c
SRCS += kernel/entry.S
SRCS += kernel/process/process.c
SRCS += kernel/process/sched.c
SRCS += kernel/process/switch.S
SRCS += kernel/process/pid.c
SRCS += kernel/syscall/syscall.c
SRCS += kernel/syscall/sysfile.c
SRCS += kernel/syscall/sysproc.c
SRCS += kernel/syscall/systime.c
SRCS += kernel/tty.c
SRCS += kernel/console.c
SRCS += kernel/lock.c
SRCS += kernel/dev/table.c
SRCS += kernel/dev/chrdev.c
SRCS += kernel/dev/blkdev.c
SRCS += kernel/dev/init.c
SRCS += drivers/virtio_disk.c
SRCS += mm/init.c
SRCS += mm/mm.c
SRCS += mm/memblock.c
SRCS += mm/pgalloc.c
SRCS += mm/pgtable.c
SRCS += mm/slab.c
SRCS += mm/vmalloc.c
SRCS += mm/ioremap.c
SRCS += mm/pagecache.c
SRCS += drivers/plic.c
SRCS += drivers/rtc.c
SRCS += drivers/uart.c
SRCS += drivers/virtio/virtio.c
SRCS += drivers/virtio/virtio_mmio.c
SRCS += drivers/virtio/virtio_virtq.c
SRCS += drivers/virtio/virtio_blk.c
SRCS += fs/tmpfs/tmpfs.c
SRCS += fs/procfs/procfs.c
SRCS += fs/brkfs/brkfs.c
SRCS += fs/brkfs/dir.c
SRCS += fs/brkfs/file.c
SRCS += fs/brkfs/inode.c
SRCS += fs/brkfs/mount.c
SRCS += fs/brkfs/super.c
SRCS += fs/init.c
SRCS += fs/pipe.c
SRCS += fs/dcache.c
SRCS += fs/file.c
SRCS += fs/inode.c
SRCS += fs/mount.c
SRCS += fs/super.c
SRCS += fs/exec.c
SRCS += fs/path.c
SRCS += fs/filesystem.c
SRCS += lib/string.c
SRCS += lib/printf.c
SRCS += lib/qsort.c
SRCS += lib/hash.c
SRCS += lib/bitmap.c
SRCS += vendor/libfdt/fdt.c
SRCS += vendor/libfdt/fdt_ro.c
SRCS += vendor/libfdt/fdt_wip.c
SRCS += vendor/libfdt/fdt_addresses.c
SRCS += vendor/libfdt/fdt_rw.c

OBJS_S := $(patsubst %.S,$(BUILD_DIR)/%.o,$(filter %.S,$(SRCS)))
OBJS_C := $(patsubst %.c,$(BUILD_DIR)/%.o,$(filter %.c,$(SRCS)))
OBJS := $(OBJS_S) $(OBJS_C)
DEPS := $(OBJS:.o=.d)

.PHONY: all brk run gdb-server gdb-client rootfs clean help

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

$(ROOTFS_IMG): scripts/mkrootfs.sh
	@./scripts/mkrootfs.sh "$(ROOTFS_IMG)"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)
