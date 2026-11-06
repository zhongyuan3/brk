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
QEMU ?= qemu-system-riscv64

QEMU_OPTS += -machine virt -nographic
