# RISC-V architecture build configuration
#
# XLEN selects the register width: 32 (rv32gc/ilp32) or 64 (rv64gc/lp64).

XLEN ?= 64

ifeq ($(XLEN),32)
# medany gives PC-relative addressing, required because the early boot
# code runs from physical addresses before paging is enabled.
CFLAGS += -march=rv32gc -mabi=ilp32 -mcmodel=medany
LDFLAGS += -m elf32lriscv
QEMU := qemu-system-riscv32
else ifeq ($(XLEN),64)
CFLAGS += -march=rv64gc -mcmodel=medany
LDFLAGS += -m elf64lriscv
QEMU := qemu-system-riscv64
else
$(error invalid XLEN '$(XLEN)' (expected 32 or 64))
endif

QEMU_OPTS := -machine virt -nographic

include scripts/riscv_toolchain.mk
