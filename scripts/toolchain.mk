# RISC-V cross toolchain auto-detection (included by top Makefile).

# Prefixes to try, highest preference first.
riscv-toolchain-prefixes := \
	riscv64-unknown-elf- \
	riscv64-elf- \
	riscv64-none-elf- \
	riscv64-linux-gnu- \
	riscv64-unknown-linux-gnu-

# Return "ok" when $(CROSS_COMPILE)gcc exists and targets riscv64.
define toolchain-ok
$(shell command -v $(CROSS_COMPILE)gcc >/dev/null 2>&1 && \
	$(CROSS_COMPILE)gcc -dumpmachine 2>/dev/null | grep -q '^riscv64' && echo ok)
endef

ifndef CROSS_COMPILE
# Do not exit 1 from shell: GNU make ignores it and leaves CROSS_COMPILE empty.
_cross-prefix := $(shell \
	for p in $(riscv-toolchain-prefixes); do \
		if command -v "$${p}gcc" >/dev/null 2>&1 && \
		   "$${p}gcc" -dumpmachine 2>/dev/null | grep -q '^riscv64'; then \
			printf '%s' "$$p"; \
			exit 0; \
		fi; \
	done)
ifeq ($(_cross-prefix),)
$(error \
	*** RISC-V cross compiler not found. \
	Tried prefixes: $(riscv-toolchain-prefixes). \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif
CROSS_COMPILE := $(_cross-prefix)
endif

ifneq ($(toolchain-ok),ok)
$(error \
	*** No usable RISC-V cross compiler '$(CROSS_COMPILE)gcc'. \
	Set CROSS_COMPILE=<prefix> or install a toolchain.)
endif
