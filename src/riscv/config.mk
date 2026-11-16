# RISC-V 64-bit architecture build configuration (rv64gc/lp64d).
#
# medany is required because the kernel is linked at a high virtual address
# but the early boot code executes from its physical load address; PC-relative
# addressing keeps both paths working.
#
# gp is never initialized, so disable the small-data optimization.  Keep the
# ABI explicit instead of relying on the toolchain default.

KBUILD_CFLAGS += -march=rv64gc -mabi=lp64d -mcmodel=medany
KBUILD_CFLAGS += -msmall-data-limit=0
KBUILD_LDFLAGS += -m elf64lriscv

PGTABLE_LEVELS ?= 3
ifeq ($(filter $(PGTABLE_LEVELS),3 4 5),)
$(error PGTABLE_LEVELS must be 3, 4, or 5)
endif
KBUILD_CFLAGS += -DCONFIG_PGTABLE_LEVELS=$(PGTABLE_LEVELS)

QEMU ?= qemu-system-riscv64

QEMU_OPTS += -machine virt -nographic
