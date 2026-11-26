# AGENTS.md

BRK is a freestanding monolithic kernel for RISC-V rv64; `ARCH=riscv` is the
only supported architecture. This tree builds the kernel only: userland,
rootfs, QEMU launch, and tests live in the sibling `../brk-user` repo.

## Build

- `make CROSS_COMPILE=riscv64-unknown-elf-` produces the debug kernel at
  `build/riscv/debug/brk`; `make BUILD=release CROSS_COMPILE=...` produces
  `build/riscv/release/brk`. The two variants coexist.
- Kernel builds abort unless `$(CC)` is a RISC-V compiler. Pass
  `CROSS_COMPILE` (or `CC=riscv64-unknown-elf-gcc`) unless the environment
  already points at the cross toolchain.
- `BUILD=release` adds `-DNDEBUG`, which compiles `kassert()` out; use debug
  when assertions matter.
- Source files are not globbed: add every `.c`/`.S` to `obj-y` in the nearest
  `Makefile`, and subdirectories to `subdir-y`. Per-file flags go in
  `ccflags-y`/`CFLAGS_<name>.o` or `asflags-y`/`AFLAGS_<name>.o`.
- The build is freestanding (`-nostdinc`, no libc); warnings are errors
  (`vendor/` opts out with `-Wno-error`), VLAs are rejected, and kernel
  functions using more than 4 KiB of stack fail the build. `WERROR=0` is a
  temporary escape hatch.
- Compiler/flag changes force a full rebuild via `.build-flags`;
  `make syncconfig` forces it explicitly. `make compile_commands` regenerates
  the gitignored `compile_commands.json` and needs `bear`. `make help` lists
  the remaining targets and variables.

## Verification

- `make format-check` must pass; `make format` rewrites files. clang-format
  (>=11) uses the Linux-style `.clang-format` here: tabs, 80 columns;
  `vendor/` and `build/` are skipped.
- `vendor/libfdt` is upstream third-party code; do not reformat it.
- There is no kernel test target. Runtime checks go through `../brk-user`:
  - `make -C ../brk-user run` builds the rootfs and kernel and boots QEMU
    `virt` with OpenSBI.
  - `make -C ../brk-user check-in-system` boots and runs the brktest suites.
  - `make -C ../brk-user gdb-server` / `gdb-client` start/connect the QEMU
    GDB stub on `:1234`.
- `../brk-user` expects this tree at `../brk` (override `KERNEL_ROOT=`) and
  needs `../brk-tools` for the rootfs. Its targets default to
  `KERNEL_BUILD=debug`; pass `KERNEL_BUILD=release` to boot a release kernel.

## Code map and conventions

- `kernel/main.c:start_kernel()` is the explicit boot init order. There are
  no initcall/linker-section hooks: add new subsystem init calls there.
  Platform drivers register with `platform_driver_register()` during those
  calls; platform devices come from the DT or explicit registration.
- `include/brk/` holds kernel headers, `include/uapi/` the shared kernel/user
  ABI (brk-user copies it at build time), and `include/` the freestanding
  libc subset. Use `<brk/...>`, `<uapi/...>`, and `<arch/...>` includes; there
  is no stdio.
- Use `printk`/`pr_*` for output and `kassert()` for invariants; plain
  `assert` is unavailable (`<assert.h>` only defines `static_assert`).
- The kernel is linked at `0xffffffff80000000` but loaded at `0x80200000`;
  early boot runs at its physical address. Keep `-mcmodel=medany` and
  `-msmall-data-limit=0` (gp is never initialized).
- `PGTABLE_LEVELS=3|4|5` selects Sv39/Sv48/Sv57 at compile time.
- Header guards are derived from the path below `include/` (private headers
  use the basename) with a leading underscore (`_BRK_INIT_H`,
  `_ARCH_ASM_PAGE_H`); `scripts/fix_header_guard.py --fix <paths>` repairs
  them.

## Syscalls and UAPI

- Adding a syscall touches four places:
  `arch/riscv/include/uapi/asm/syscall.h` (`__NR_*`, dense numbering, update
  `__NR_SYSCALL_MAX`), `include/uapi/syscall.h` (`SYS_*`),
  `include/brk/syscall.h` (`sys_*` prototype), and `kernel/syscall.c`
  (`systable`; a `static_assert` checks its size).
- Handlers are `long sys_foo(const struct syscall_ctx *ctx)` and read
  arguments through the `syscall_arg_*()` helpers.
- Syscall numbers are internal and get repacked; use the `SYS_*` names rather
  than literals. Rebuild `../brk-user` after any UAPI change.
- UAPI headers must stay self-contained: use `__brk_*` types and other
  `<uapi/...>` headers, never `<stdint.h>` or kernel-internal headers.
  `<brk/types.h>` maps the ABI types for kernel code.

## Commits

- Conventional Commits are enforced by `.githooks/commit-msg` (enable with
  `make hooks`). Allowed types: feat, fix, refactor, chore, docs, build, perf,
  test, style, ci, revert; subject <=72 chars, no trailing period, blank line
  before body.
