# RISC-V 64-bit architecture build configuration (rv64gc/lp64).
#
# medany gives PC-relative addressing, required because the early boot
# code runs from physical addresses before paging is enabled.

CFLAGS += -march=rv64gc -mcmodel=medany
LDFLAGS += -m elf64lriscv
QEMU := qemu-system-riscv64

QEMU_OPTS := -machine virt -nographic
