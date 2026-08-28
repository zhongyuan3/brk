# RISC-V architecture build configuration

CFLAGS += -march=rv64gc -mcmodel=medany

QEMU := qemu-system-riscv64
QEMU_OPTS := -machine virt -nographic

include scripts/riscv_toolchain.mk
