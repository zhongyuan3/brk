CROSS_COMPILE ?= riscv64-unknown-elf-

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
AS      := $(CROSS_COMPILE)as
AR      := $(CROSS_COMPILE)ar
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
GDB     := gdb-multiarch

CFLAGS := -O0 -ggdb -gdwarf-2 -Wall -Wextra -Werror -Wno-unused-parameter \
-Wno-unknown-attributes -Wno-main -fno-omit-frame-pointer -march=rv64gc \
-mcmodel=medany -ffreestanding -fno-common -nostdlib -fno-stack-protector \
-fno-pie -no-pie

CFLAGS += -I./include -I./lib/libfdt -MMD

ifeq ($(V),1)
Q :=
else
Q := @
endif

SRCS := \
	boot/head.S \
	boot/setup.c \
	kernel/dtb.c \
	kernel/main.c \
	kernel/panic.c \
	kernel/printk.c \
	kernel/trap.c \
	mm/init.c \
	mm/memblock.c \
	mm/pgalloc.c \
	mm/pgtable.c \
	mm/slab.c \
	mm/vmalloc.c \
	mm/ioremap.c \
	lib/string.c \
	lib/printf.c \
	lib/libfdt/fdt.c \
	lib/libfdt/fdt_ro.c \
	lib/libfdt/fdt_wip.c \
	lib/libfdt/fdt_addresses.c \
	lib/libfdt/fdt_rw.c

OBJS := $(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS)))
DEPS := $(OBJS:.o=.d)

AOSD_LD_S    := aosd.ld.S
AOSD_LD      := aosd.ld
AOSD_ELF     := aosd.elf
AOSD_DIS     := aosd.dis

DTB          := virt.dtb
DTS          := virt.dts
ROOTFS_IMG   := rootfs.img

QEMU           := qemu-system-riscv64
QEMU_COMMON    := -machine virt -nographic -m 128M -smp 1 -bios default \
-drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0 \
-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

.PHONY: all aosd clean gdb-server gdb-client dis dts rootfs run

all: aosd

aosd: $(AOSD_ELF)

clean:
	$(Q)$(RM) $(AOSD_ELF) $(AOSD_LD) $(AOSD_DIS) $(DTB) $(DTS)
	$(Q)find . -name "*.d" -delete
	$(Q)find . -name "*.o" -delete

run: $(AOSD_ELF) $(ROOTFS_IMG)
	$(Q)$(QEMU) $(QEMU_COMMON) -kernel $(AOSD_ELF)

gdb-server: $(AOSD_ELF) $(ROOTFS_IMG)
	$(Q)$(QEMU) $(QEMU_COMMON) -S -s -kernel $(AOSD_ELF)

gdb-client: $(AOSD_ELF)
	$(Q)$(GDB) --tui -quiet -ex "target remote :1234" $(AOSD_ELF)

dis: $(AOSD_ELF)
	$(Q)$(OBJDUMP) -d -S -l --source-comment $< > $(AOSD_DIS)

dts: $(DTS)

rootfs: $(ROOTFS_IMG)

$(AOSD_LD): $(AOSD_LD_S)
	$(Q)$(CPP) $(CFLAGS) -o $@ $<

$(AOSD_ELF): $(OBJS) $(AOSD_LD)
	$(Q)echo "LD $@"
	$(Q)$(LD) -z max-page-size=4096 -T $(AOSD_LD) -o $@ $(OBJS)

$(DTB):
	$(Q)$(QEMU) $(QEMU_COMMON) -M dumpdtb=$@

$(DTS): $(DTB)
	$(Q)dtc -I dtb -O dts $< > $@

$(ROOTFS_IMG):
	$(Q)dd if=/dev/zero of=$@ bs=1M count=64 status=progress
	$(Q)mkfs.ext4 -F -L AOSD_ROOT $@

-include $(DEPS)

%.o: %.c
	$(Q)echo "CC $@"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

%.o: %.S
	$(Q)echo "CC $@"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<
