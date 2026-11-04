BUILD_ROOT ?= build
USER_ROOT ?= ../brk-user
BOARD_BUILD_ROOT ?= $(BUILD_ROOT)/$(ARCH)

MAKEFLAGS += --no-print-directory

ifeq ($(V),1)
Q :=
else
MAKEFLAGS += --silent
Q := @
endif

ARCH ?= riscv

CFLAGS :=
LDFLAGS :=
LDFLAGS_FINAL :=

ifeq ($(ARCH),riscv)
include src/arch/$(ARCH)/config.mk
else
$(error Unsupported architecture '$(ARCH)')
endif

BUILD ?= debug
WERROR ?= 1
RAM ?= 128M
CPU ?= 3
KERNEL_BUILD_ROOT ?= $(BUILD_ROOT)/$(ARCH)/$(BUILD)
ROOTFS_IMG ?= $(BOARD_BUILD_ROOT)/rootfs.img

CROSS_COMPILE ?=

# Respect CC/LD/... from the environment or the command line, but replace
# make's built-in defaults (cc/ld/...) with the cross-toolchain equivalents.
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CPP := $(CC) -E
ifeq ($(origin LD),default)
LD := $(CROSS_COMPILE)ld
endif
ifeq ($(origin AS),default)
AS := $(CROSS_COMPILE)as
endif
ifeq ($(origin AR),default)
AR := $(CROSS_COMPILE)ar
endif
OBJCOPY ?= $(CROSS_COMPILE)objcopy
OBJDUMP ?= $(CROSS_COMPILE)objdump
SIZE ?= $(CROSS_COMPILE)size
GDB ?= $(CROSS_COMPILE)gdb

CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Wundef
CFLAGS += -Wvla
CFLAGS += -Wformat=2
CFLAGS += -Wimplicit-fallthrough
CFLAGS += -Wframe-larger-than=4096
CFLAGS += -Wstack-usage=4096
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif
CFLAGS += -ffreestanding
CFLAGS += -fno-common
CFLAGS += -fno-strict-aliasing
CFLAGS += -fno-delete-null-pointer-checks
CFLAGS += -nostdlib
CFLAGS += -fno-stack-protector
CFLAGS += -fno-pie
CFLAGS += -no-pie

ifeq ($(BUILD),debug)
CFLAGS += -Og
CFLAGS += -ggdb
CFLAGS += -gdwarf-4
CFLAGS += -fno-omit-frame-pointer
else ifeq ($(BUILD),release)
CFLAGS += -O2
CFLAGS += -DNDEBUG
CFLAGS += -fomit-frame-pointer
CFLAGS += -fno-semantic-interposition
CFLAGS += -fno-unwind-tables
CFLAGS += -fno-asynchronous-unwind-tables
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections
LDFLAGS_FINAL += --gc-sections
else
$(error Unknown build type '$(BUILD)')
endif

CFLAGS += -std=gnu11

CFLAGS += -nostdinc
CFLAGS += -MMD
CFLAGS += -MP
CFLAGS += -Isrc/arch/$(ARCH)/include
CFLAGS += -Isrc/include
CFLAGS += -Isrc/include/freestanding
CFLAGS += -Ivendor/libfdt

LDFLAGS += -z max-page-size=4096

# User-supplied additions; use these instead of overriding CFLAGS/LDFLAGS,
# which would discard the flags assembled above.
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# Compiler runtime helpers (e.g. 128-bit division) not emitted natively.
LIBGCC := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name 2>/dev/null)

# Goals that do not build kernel objects do not need the RISC-V toolchain.
# An empty MAKECMDGOALS means the default "all" target.
ifeq ($(MAKECMDGOALS),)
KERNEL_GOALS := all
else
KERNEL_GOALS := $(filter-out clean clean-kernel help headers-check \
	format format-check dtb dts rootfs,$(MAKECMDGOALS))
endif

# Fail early with a useful message instead of a wall of compiler errors.
ifneq ($(KERNEL_GOALS),)
ifeq ($(shell $(CC) -dumpmachine 2>/dev/null | grep -c '^riscv'),0)
$(error $(CC) is not a RISC-V compiler; set CROSS_COMPILE (e.g. riscv64-unknown-elf-))
endif
ifeq ($(strip $(LIBGCC)),)
$(error Cannot find libgcc for $(CC); set CROSS_COMPILE (e.g. riscv64-unknown-elf-))
endif
endif

# Rebuild objects when the compiler or flags change.  The stamp is only
# rewritten when its contents differ, so unchanged builds stay incremental.
FLAGS_STAMP := $(KERNEL_BUILD_ROOT)/.build-flags
FLAGS_SIGNATURE := ARCH=$(ARCH) BUILD=$(BUILD) CC=$(CC) LD=$(LD) \
	CFLAGS=$(CFLAGS) LDFLAGS=$(LDFLAGS) \
	LDFLAGS_FINAL=$(LDFLAGS_FINAL) LIBGCC=$(LIBGCC) \
	LINK_CHECK=$(LINK_CHECK)
ifneq ($(KERNEL_GOALS),)
FLAGS_OLD := $(shell cat $(FLAGS_STAMP) 2>/dev/null)
ifneq ($(FLAGS_SIGNATURE),$(FLAGS_OLD))
$(shell mkdir -p $(dir $(FLAGS_STAMP)); printf '%s\n' '$(FLAGS_SIGNATURE)' > $(FLAGS_STAMP))
endif
endif
export ARCH
export BUILD_ROOT
export CROSS_COMPILE CC LD AS AR CFLAGS LDFLAGS

LINKER_SCRIPT_SRC := src/arch/$(ARCH)/kernel/brk.ld.S
LINKER_SCRIPT := $(KERNEL_BUILD_ROOT)/brk.ld
LINKER_SCRIPT_DEP := $(LINKER_SCRIPT).d
KERNEL_ELF := $(KERNEL_BUILD_ROOT)/brk
KERNEL_MAP := $(KERNEL_ELF).map

LDFLAGS_FINAL += -Map=$(KERNEL_MAP)

# Opt-in: report sections that are not explicitly placed by the linker
# script.  This is noisy for the debug sections in libgcc, so keep it off by
# default and enable it with LINK_CHECK=1.
ifeq ($(LINK_CHECK),1)
LDFLAGS_FINAL += --orphan-handling=warn
endif

core-y := src
core-y += vendor

BUILT_IN_OBJS := $(addprefix $(KERNEL_BUILD_ROOT)/,$(patsubst %,%/built-in.o,$(core-y)))

QEMU_OPTS += -m $(RAM)
QEMU_OPTS += -smp $(CPU)
QEMU_OPTS += -bios default
QEMU_OPTS += -drive file=$(ROOTFS_IMG),if=none,format=raw,id=x0
QEMU_OPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMU_OPTS += -global virtio-mmio.force-legacy=false

# Set QEMU_NET=0 to boot without a network device.  The NIC sits on the
# second virtio-mmio bus because virtio-mmio-bus.0 is taken by the disk.
QEMU_NET ?= 1
ifeq ($(QEMU_NET),1)
QEMU_OPTS += -netdev user,id=net0
QEMU_OPTS += -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.1

# Optional pcap capture, e.g. make run QEMU_NET_DUMP=/tmp/brk-net.pcap
ifneq ($(QEMU_NET_DUMP),)
QEMU_OPTS += -object filter-dump,id=netdump,netdev=net0,file=$(QEMU_NET_DUMP)
endif
endif

QEMU_OPTS += $(EXTRA_QEMU_OPTS)

.DELETE_ON_ERROR:

.PHONY: all brk run gdb-server gdb-client rootfs clean clean-kernel \
	headers-check check format format-check compile_commands size disasm \
	help FORCE

all: brk

brk: $(KERNEL_ELF)

headers-check:
	$(Q)CC="$(CC)" ARCH="$(ARCH)" ./scripts/check-headers.sh

check: headers-check
	$(Q)$(MAKE) BUILD=debug brk
	$(Q)$(MAKE) BUILD=release brk

format:
	$(Q)./scripts/format-src.sh

format-check:
	$(Q)./scripts/format-src.sh --check

compile_commands:
	@command -v bear >/dev/null 2>&1 || \
		{ echo "error: bear not found in PATH" >&2; exit 1; }
	$(Q)bear --output compile_commands.json -- $(MAKE) -B brk

size: $(KERNEL_ELF)
	$(Q)$(SIZE) $<

disasm: $(KERNEL_ELF)
	$(Q)$(OBJDUMP) -d $< > $<.dis
	$(Q)echo "  DIS     $<.dis"

help:
	$(Q)echo "Targets:"
	$(Q)echo "  all / brk              Build the kernel (SMP capable)"
	$(Q)echo "  headers-check          Verify UAPI/kernel header layering and ABI"
	$(Q)echo "  check                  headers-check + debug/release builds"
	$(Q)echo "  format                 Format src/ with clang-format"
	$(Q)echo "  format-check           Check src/ formatting (no writes)"
	$(Q)echo "  compile_commands       Regenerate compile_commands.json (needs bear)"
	$(Q)echo "  run                    Build rootfs + boot kernel in QEMU"
	$(Q)echo "  debug                  QEMU with interrupt tracing"
	$(Q)echo "  gdb-server             QEMU + GDB stub"
	$(Q)echo "  gdb-client             Connect GDB to :1234"
	$(Q)echo "  rootfs                 Build the root filesystem via $(USER_ROOT)"
	$(Q)echo "  dtb / dts              Dump the QEMU virt device tree"
	$(Q)echo "  size / disasm          Show kernel size / disassembly"
	$(Q)echo "  clean                  Remove all build outputs"
	$(Q)echo "  clean-kernel           Remove current variant build directory"
	$(Q)echo ""
	$(Q)echo "Configurable vars:"
	$(Q)echo "  ARCH=<name>"
	$(Q)echo "  CROSS_COMPILE=<prefix>"
	$(Q)echo "  BUILD={debug|release}  (default: debug)"
	$(Q)echo "  BUILD_ROOT=<path>      (default: build)"
	$(Q)echo "  USER_ROOT=<path>       (default: ../brk-user)"
	$(Q)echo "  CPU=<n>                (default 3)"
	$(Q)echo "  RAM=<size>             (default 128M)"
	$(Q)echo "  WERROR={0|1}           (default: 1)"
	$(Q)echo "  V=1                    Show full build commands"
	$(Q)echo "  QEMU_NET={0|1}         (default: 1)"
	$(Q)echo "  QEMU_NET_DUMP=<file>   Capture QEMU network traffic"
	$(Q)echo "  EXTRA_CFLAGS=...       Additional compiler flags"
	$(Q)echo "  EXTRA_LDFLAGS=...      Additional linker flags"
	$(Q)echo "  EXTRA_QEMU_OPTS=...    Additional QEMU flags"
	$(Q)echo "  LINK_CHECK={0|1}       Warn about linker orphan sections"

$(LINKER_SCRIPT): $(LINKER_SCRIPT_SRC) $(FLAGS_STAMP) \
	$(if $(wildcard $(LINKER_SCRIPT_DEP)),,FORCE)
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "  CPP     $@"
	$(Q)$(CPP) $(filter-out -MMD -MP,$(CFLAGS)) \
		-MD -MF $(LINKER_SCRIPT_DEP) -MT $@ -o $@ $<

$(KERNEL_ELF): $(BUILT_IN_OBJS) $(LINKER_SCRIPT) $(FLAGS_STAMP)
	$(Q)echo "  LD      $@"
	$(Q)$(LD) $(LDFLAGS) $(LDFLAGS_FINAL) -T $(LINKER_SCRIPT) -static -o $@ $(BUILT_IN_OBJS) $(LIBGCC)

$(KERNEL_BUILD_ROOT)/%/built-in.o: FORCE
	$(Q)$(MAKE) -f scripts/Makefile.build build=$* \
		KERNEL_BUILD_ROOT=$(KERNEL_BUILD_ROOT) FLAGS_STAMP=$(FLAGS_STAMP)

# Single-object convenience targets.  They build the whole directory, which
# is cheap and keeps the recursive Makefile logic in one place.
$(KERNEL_BUILD_ROOT)/src/%.o: FORCE
	$(Q)$(MAKE) -f scripts/Makefile.build \
		build=src/$(patsubst %/,%,$(dir $*)) \
		KERNEL_BUILD_ROOT=$(KERNEL_BUILD_ROOT) FLAGS_STAMP=$(FLAGS_STAMP)

$(KERNEL_BUILD_ROOT)/vendor/%.o: FORCE
	$(Q)$(MAKE) -f scripts/Makefile.build \
		build=vendor/$(patsubst %/,%,$(dir $*)) \
		KERNEL_BUILD_ROOT=$(KERNEL_BUILD_ROOT) FLAGS_STAMP=$(FLAGS_STAMP)



run: rootfs $(KERNEL_ELF)
	$(Q)$(QEMU) $(QEMU_OPTS) -kernel $(KERNEL_ELF)

gdb-server: rootfs $(KERNEL_ELF)
	$(Q)$(QEMU) $(QEMU_OPTS) -S -s -kernel $(KERNEL_ELF)

gdb-client: $(KERNEL_ELF)
	$(Q)$(GDB) --tui -quiet -ex "target remote :1234" $(KERNEL_ELF)

rootfs:
	$(Q)KERNEL_ROOT="$(CURDIR)" $(USER_ROOT)/scripts/mkrootfs.sh "$(ROOTFS_IMG)"

$(ROOTFS_IMG): $(USER_ROOT)/scripts/mkrootfs.sh
	$(Q)KERNEL_ROOT="$(CURDIR)" $(USER_ROOT)/scripts/mkrootfs.sh "$@"

clean:
	$(Q)$(RM) -r $(BUILD_ROOT)

clean-kernel:
	$(Q)$(RM) -r $(KERNEL_BUILD_ROOT)

.PHONY: debug
debug: rootfs $(KERNEL_ELF)
	$(Q)$(QEMU) $(QEMU_OPTS) -kernel $(KERNEL_ELF) -d int

DTB := $(BOARD_BUILD_ROOT)/qemu_virt.dtb
DTS := $(BOARD_BUILD_ROOT)/qemu_virt.dts

.PHONY: dtb dts
dtb: $(DTB)

$(DTB):
	$(Q)mkdir -p $(dir $@)
	$(Q)$(QEMU) -M virt,dumpdtb=$@

dts: $(DTS)

$(DTS): $(DTB)
	@command -v dtc >/dev/null 2>&1 || \
		{ echo "error: dtc not found in PATH" >&2; exit 1; }
	$(Q)dtc -I dtb -O dts $< -o $@

-include $(LINKER_SCRIPT_DEP)
