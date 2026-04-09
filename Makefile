CROSS_COMPILE ?= riscv64-unknown-elf-

CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
GDB := gdb-multiarch

BUILD ?= DEBUG

ifeq ($(BUILD), DEBUG)
OPT := 0
else
ifeq ($(BUILD), RELEASE)
OPT := 2
else
$(error unknown build type $(BUILD))
endif
endif

CPU ?= 3

RAM ?= 128M

CFLAGS := -O$(OPT) -ggdb -gdwarf-2 -Wall -Wextra -Werror
CFLAGS += -Wno-unused-parameter -Wno-unknown-attributes -Wno-main
CFLAGS += -march=rv64gc
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib
CFLAGS += -fno-omit-frame-pointer
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pie -no-pie
CFLAGS += -MMD
CFLAGS += -I./include -I./lib/libfdt -I./lib/lwext4/include

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
fs/ext4/direntry.c \
fs/ext4/file.c \
fs/ext4/inode.c \
fs/ext4/mount.c \
fs/ext4/super.c \
fs/tmpfs/tmpfs.c \
fs/dcache.c \
fs/file.c \
fs/inode.c \
fs/mount.c \
fs/pipe.c \
fs/super.c \
fs/exec.c \
fs/path.c \
fs/filesystem.c \
lib/string.c \
lib/printf.c \
lib/qsort.c \
lib/libfdt/fdt.c \
lib/libfdt/fdt_ro.c \
lib/libfdt/fdt_wip.c \
lib/libfdt/fdt_addresses.c \
lib/libfdt/fdt_rw.c \
$(wildcard lib/lwext4/src/*.c)

BRK_LD_S := kernel/brk.ld.S
ROOTFS_IMG := rootfs.img

QEMU := qemu-system-riscv64
QEMU_COMMON := -machine virt -nographic
QEMU_COMMON += -m $(RAM)
QEMU_COMMON += -smp $(CPU)
QEMU_COMMON += -bios default
QEMU_COMMON += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_COMMON += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_COMMON += -global virtio-mmio.force-legacy=false

OBJS := $(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS)))
DEPS := $(OBJS:.o=.d)
BRK_LD := kernel/brk.ld
BRK_ELF := brk.elf

.PHONY: all brk clean gdb-server gdb-client rootfs run

all: brk

brk: $(BRK_ELF)

gdb-server: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -S -s -kernel $(BRK_ELF)

gdb-client: $(BRK_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(BRK_ELF)

rootfs: $(ROOTFS_IMG)

run: $(BRK_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -kernel $(BRK_ELF)

$(BRK_LD): $(BRK_LD_S)
	$(CPP) $(CFLAGS) -o $@ $<

$(BRK_ELF): $(OBJS) $(BRK_LD)
	$(LD) -z max-page-size=4096 -T $(BRK_LD) -static -o $@ $(OBJS)

$(ROOTFS_IMG):
	dd if=/dev/zero of=$@ bs=1M count=64 status=progress
	mkfs.ext4 -F -L BRK_ROOT -m 0 $@

clean:
	$(RM) $(BRK_ELF) $(BRK_LD) $(BRK_DIS) $(DTB) $(DTS)
	find . -name "*.d" -delete
	find . -name "*.o" -delete

-include $(DEPS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<
