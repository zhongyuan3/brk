riscv-toolchain-prefixes := \
	riscv64-unknown-elf- \
	riscv64-elf- \
	riscv64-none-elf- \
	riscv64-linux-gnu- \
	riscv64-unknown-linux-gnu-

TOOLCHAIN_CACHE := $(BUILD_ROOT)/.toolchain-$(ARCH).mk
DETECT_SCRIPT := scripts/detect-riscv-toolchain.sh
__need_gdb := $(strip $(filter gdb-client,$(MAKECMDGOALS)))

ifneq ($(wildcard $(TOOLCHAIN_CACHE)),)
include $(TOOLCHAIN_CACHE)
endif

# Cache hit: validate with wildcard only (no shell).
__cross_ok := $(and $(CROSS_GCC),$(wildcard $(CROSS_GCC)))
ifneq ($(CROSS_COMPILE),)
__cross_ok := $(and $(__cross_ok),$(filter $(CROSS_COMPILE)%,$(notdir $(CROSS_GCC))))
endif

ifeq ($(__cross_ok),)
__cross_detected := $(shell CROSS_COMPILE='$(CROSS_COMPILE)' \
	$(DETECT_SCRIPT) '$(TOOLCHAIN_CACHE)' cross)
ifeq ($(__cross_detected),)
$(error \
	*** RISC-V cross compiler not found. \
	Tried prefixes: $(riscv-toolchain-prefixes). \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif
include $(TOOLCHAIN_CACHE)
endif

ifndef CROSS_COMPILE
$(error \
	*** RISC-V cross compiler not configured. \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif

ifneq ($(__need_gdb),)
ifeq ($(wildcard $(GDB)),)
__gdb_detected := $(shell CROSS_COMPILE='$(CROSS_COMPILE)' \
	$(DETECT_SCRIPT) '$(TOOLCHAIN_CACHE)' gdb '$(XLEN)')
ifeq ($(__gdb_detected),)
$(error \
	*** GDB with RISC-V support not found. \
	Tried: $(CROSS_COMPILE)gdb gdb-multiarch. \
	Set GDB=<path> or install a RISC-V-capable GDB.)
endif
include $(TOOLCHAIN_CACHE)
endif
ifeq ($(wildcard $(GDB)),)
$(error \
	*** No usable RISC-V GDB '$(GDB)'. \
	Set GDB=<path> or install a RISC-V-capable GDB.)
endif
endif
