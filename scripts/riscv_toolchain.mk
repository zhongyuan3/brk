riscv-toolchain-prefixes := \
	riscv64-unknown-elf- \
	riscv64-elf- \
	riscv64-none-elf- \
	riscv64-linux-gnu- \
	riscv64-unknown-linux-gnu-

BRK_TOOLCHAIN_CACHE := $(BRK_BUILD_ROOT)/.toolchain-$(ARCH).mk
BRK_DETECT_SCRIPT := $(BRK_ROOT)/scripts/detect-riscv-toolchain.sh
_brk_need_gdb := $(strip $(filter gdb-client gdb-client-smp,$(MAKECMDGOALS)))

ifneq ($(wildcard $(BRK_TOOLCHAIN_CACHE)),)
include $(BRK_TOOLCHAIN_CACHE)
endif

# Cache hit: validate with wildcard only (no shell).
_brk_cross_ok := $(and $(BRK_CROSS_GCC),$(wildcard $(BRK_CROSS_GCC)))
ifneq ($(CROSS_COMPILE),)
_brk_cross_ok := $(and $(_brk_cross_ok),$(filter $(CROSS_COMPILE)%,$(notdir $(BRK_CROSS_GCC))))
endif

ifeq ($(_brk_cross_ok),)
_brk_cross_detected := $(shell CROSS_COMPILE='$(CROSS_COMPILE)' \
	$(BRK_DETECT_SCRIPT) '$(BRK_TOOLCHAIN_CACHE)' cross)
ifeq ($(_brk_cross_detected),)
$(error \
	*** RISC-V cross compiler not found. \
	Tried prefixes: $(riscv-toolchain-prefixes). \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif
include $(BRK_TOOLCHAIN_CACHE)
endif

ifndef CROSS_COMPILE
$(error \
	*** RISC-V cross compiler not configured. \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif

ifneq ($(_brk_need_gdb),)
ifeq ($(wildcard $(GDB)),)
_brk_gdb_detected := $(shell CROSS_COMPILE='$(CROSS_COMPILE)' \
	$(BRK_DETECT_SCRIPT) '$(BRK_TOOLCHAIN_CACHE)' gdb)
ifeq ($(_brk_gdb_detected),)
$(error \
	*** GDB with RISC-V support not found. \
	Tried: $(CROSS_COMPILE)gdb gdb-multiarch. \
	Set GDB=<path> or install a RISC-V-capable GDB.)
endif
include $(BRK_TOOLCHAIN_CACHE)
endif
ifeq ($(wildcard $(GDB)),)
$(error \
	*** No usable RISC-V GDB '$(GDB)'. \
	Set GDB=<path> or install a RISC-V-capable GDB.)
endif
endif
