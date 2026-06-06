# RISC-V architecture build configuration

BRK_ARCH_FLAGS := -march=rv64gc -mcmodel=medany
QEMU := qemu-system-riscv64
QEMU_OPTS := -machine virt -nographic

include $(BRK_ROOT)/scripts/toolchain.mk
