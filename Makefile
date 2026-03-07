CROSS_COMPILE ?= riscv64-unknown-elf-

CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
GDB := gdb-multiarch

CFLAGS := -O0 -ggdb -gdwarf-2 -Wall -Wextra -Werror
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
kernel/sched/sched.c \
kernel/sched/task.c \
kernel/sched/switch.S \
kernel/sched/pid.c \
kernel/syscall.c \
kernel/sysfile.c \
kernel/sysproc.c \
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

AOSD_LD_S := kernel/aosd.ld.S
ROOTFS_IMG := rootfs.img

QEMU := qemu-system-riscv64
QEMU_COMMON := -machine virt -nographic
QEMU_COMMON += -m 128M
QEMU_COMMON += -smp 3
QEMU_COMMON += -bios default
QEMU_COMMON += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_COMMON += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_COMMON += -global virtio-mmio.force-legacy=false
QEMU_COMMON += -d guest_errors

OBJS := $(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS)))
DEPS := $(OBJS:.o=.d)
AOSD_LD := kernel/aosd.ld
AOSD_ELF := aosd.elf
AOSD_DIS := aosd.dis
DTB := virt.dtb
DTS := virt.dts

.PHONY: all aosd clean gdb-server gdb-client dis dts rootfs run

all: aosd

aosd: $(AOSD_ELF)

gdb-server: $(AOSD_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -S -s -kernel $(AOSD_ELF)

gdb-client: $(AOSD_ELF)
	$(GDB) --tui -quiet -ex "target remote :1234" $(AOSD_ELF)

dis: $(AOSD_ELF)
	$(OBJDUMP) -d -S -l $< > $(AOSD_DIS)

dts: $(DTS)

rootfs: $(ROOTFS_IMG)

run: $(AOSD_ELF) $(ROOTFS_IMG)
	$(QEMU) $(QEMU_COMMON) -kernel $(AOSD_ELF)

$(AOSD_LD): $(AOSD_LD_S)
	$(CPP) $(CFLAGS) -o $@ $<

$(AOSD_ELF): $(OBJS) $(AOSD_LD)
	$(LD) -z max-page-size=4096 -T $(AOSD_LD) -static -o $@ $(OBJS)

$(DTB):
	$(QEMU) $(QEMU_COMMON) -M dumpdtb=$@

$(DTS): $(DTB)
	dtc -I dtb -O dts $< > $@

$(ROOTFS_IMG):
	dd if=/dev/zero of=$@ bs=1M count=64 status=progress
	mkfs.ext4 -F -L AOSD_ROOT -m 0 $@

-include $(DEPS)

boot/%.o: boot/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

boot/%.o: boot/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

drivers/%.o: drivers/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

fs/%.o: fs/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

fs/%.o: fs/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

kernel/%.o: kernel/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

lib/%.o: lib/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

mm/%.o: mm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

mm/%.o: mm/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

USER_CFLAGS := -O0 -ggdb -gdwarf-2 -Wall -Wextra -Werror
USER_CFLAGS += -Wno-unused-parameter -Wno-unknown-attributes -Wno-main
USER_CFLAGS += -march=rv64gc
USER_CFLAGS += -mabi=lp64d
USER_CFLAGS += -mcmodel=medlow
USER_CFLAGS += -ffreestanding -nostdlib -fno-common
USER_CFLAGS += -fno-omit-frame-pointer
USER_CFLAGS += -fno-stack-protector
USER_CFLAGS += -fno-pie -no-pie
USER_CFLAGS += -MMD
USER_CFLAGS += -I./include

USER_LD_S := user/user.ld.S

USER_PROG_SRCS := \
user/cat.c \
user/echo.c \
user/ls.c \
user/mkdir.c \
user/rm.c \
user/sh.c

USER_LIB_OBJS := \
user/printf.o \
user/string.o \
user/qsort.o \
user/ulib.o

USER_PROG_OBJS := $(patsubst %.c,%.o,$(USER_PROG_SRCS))
USER_PROG := $(patsubst %.c,%,$(USER_PROG_SRCS))

USER_LD := user/user.ld
USER_LIB := user/libulib.a

clean:
	$(RM) $(AOSD_ELF) $(AOSD_LD) $(AOSD_DIS) $(DTB) $(DTS)
	$(RM) $(USER_LIB) $(USER_PROG) $(USER_LD)
	find . -name "*.d" -delete
	find . -name "*.o" -delete

.PHONY: user
user: $(USER_LIB) $(USER_PROG) $(USER_PROG_OBJS)

user/%: user/%.o $(USER_LIB) $(USER_LD)
	$(LD) -z max-page-size=4096 -T $(USER_LD) -static -o $@ $< -L./user -lulib

$(USER_LIB): $(USER_LIB_OBJS)
	$(AR) rcs $@ $^

user/%.o: user/%.c
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(USER_LD): $(USER_LD_S)
	$(CPP) $(USER_CFLAGS) -o $@ $<

user/printf.o: lib/printf.c
	$(CC) $(USER_CFLAGS) -c -o $@ $<

user/qsort.o: lib/qsort.c
	$(CC) $(USER_CFLAGS) -c -o $@ $<

user/string.o: lib/string.c
	$(CC) $(USER_CFLAGS) -c -o $@ $<
