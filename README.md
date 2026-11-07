# BRK

**B**arely **R**unning **K**ernel

> _"Yes, it's a BRK. But it's MY BRK."_

![Build Status](https://img.shields.io/badge/build-probably_passing-yellow)
![Language](https://img.shields.io/badge/language-C-blue)
![Arch](https://img.shields.io/badge/target-RISCV--64-orange)
![Mood](https://img.shields.io/badge/mood-panic%20but_happy-red)
![License](https://img.shields.io/badge/license-MIT-green)

---

## Table of Contents

- [BRK](#brk)
  - [Table of Contents](#table-of-contents)
  - [What the heck is this?](#what-the-heck-is-this)
  - [Inspiration \& Origins](#inspiration--origins)
  - [Build \& Run (At Your Own Risk)](#build--run-at-your-own-risk)
    - [Prerequisites](#prerequisites)
    - [Quick Start](#quick-start)
    - [Useful Helpers](#useful-helpers)
    - [Useful Variables](#useful-variables)
  - [Kernel Architecture](#kernel-architecture)
  - [License](#license)

---

## What the heck is this?

BRK is a monolithic kernel written entirely in C, driven purely by personal curiosity (and maybe a slight touch of madness).

Why call it the "Barely Running Kernel"? Because for 90% of the development cycle, it is either stuck in an infinite loop inside QEMU or throwing a spectacular Kernel Panic. But for that glorious 10% of the time when it successfully boots, sets up the page tables, and drops to a shell prompt... it is technically running.

While it currently targets the **RISC-V (rv64)** architecture, the underlying code is being written with abstraction in mind. Good kernel code shouldn't be held hostage by a specific instruction set.

## Inspiration & Origins

I'm not trying to reinvent the wheel here. BRK stands firmly on the shoulders of giants. If you are familiar with the following projects, you will immediately recognize their DNA in my codebase:

- 🐮 **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv) — The Bootstrapper:** In the early days, xv6 was my absolute guide. It taught me how to survive the bare-metal RISC-V environment: machine-mode transitions, initial page table setup, and basic trap handling. BRK's early memory layout and boot flow owe a lot to MIT's legendary teaching OS.
- 🐧 **[Linux Kernel](https://github.com/torvalds/linux) — The North Star:** As BRK began to outgrow its xv6 training wheels, I started looking towards the ultimate monolithic kernel for inspiration. From subsystem boundaries and robust linked list macros to the philosophy behind the Slab allocator, I am trying to understand Linux's design and reimplement those concepts in my own, much simpler way.

**In short: xv6 taught me how to start; Linux taught me how far I need to go.**

## Build & Run (At Your Own Risk)

If you are morbidly curious to see what a "barely running kernel" looks like, you can try spinning it up in a RISC-V environment.

### Prerequisites

- `riscv64-unknown-elf-toolchain` (or any RISC-V cross-compiler)
- `QEMU` (System mode, `qemu-system-riscv64`)

### Quick Start

```sh
# Build the kernel (debug by default)
make CROSS_COMPILE=riscv64-unknown-elf-

# Build and boot in QEMU (also rebuilds the root filesystem)
make run CROSS_COMPILE=riscv64-unknown-elf-

# Release build
make BUILD=release CROSS_COMPILE=riscv64-unknown-elf-
```

### Useful Helpers

```sh
make help           # list all available targets
make rootfs         # build the root filesystem
make size           # show kernel size
make disasm         # disassemble the kernel
```

### Useful Variables

| Variable                                 | Description                                               |
| ---------------------------------------- | --------------------------------------------------------- |
| `BUILD=debug\|release`                   | Build type                                                |
| `SMP=<n>` / `M=<size>`                   | Configure QEMU CPU count and RAM size                     |
| `V=1`                                    | Show full compiler/linker commands                        |
| `EXTRA_CFLAGS=...` / `EXTRA_LDFLAGS=...` | Temporarily append compile/link flags                     |
| `WERROR=0`                               | Keep warnings non-fatal while bringing up a new toolchain |

## Kernel Architecture

BRK follows a small, Linux-inspired device model:

```text
FDT
 └─ device_node / struct resource
     └─ platform device
         └─ bus match
             └─ driver probe
```

The virtio stack uses the same path one level deeper:

```text
virtio,mmio platform device
 └─ virtio-mmio transport driver
     └─ virtio device (child, on virtio_bus)
         └─ virtio-blk / virtio-net driver probe
```

Boot is split into explicit stages:

| Stage | Contents |
| ----- | -------- |
| before initcalls | paging, memory, CPU discovery, timebase |
| `core` | device/bus/driver/IRQ core, slab caches, network core |
| `postcore` | OF `device_node` tree, IRQ domains, platform device population |
| `subsys` | platform and transport drivers (`ns16550a`, goldfish RTC, virtio-mmio) |
| `device` | leaf drivers (`virtio-blk`, `virtio-net`) |
| `late` | deferred-probe retry and self-tests |

Filesystems start after `do_initcalls()`, from task context.

A few dependencies are deliberately handled early and are documented in the
code:

- **IRQ controllers** probe at `postcore` so IRQ domains exist before normal
  devices resolve their interrupts in probe context.
- **RTC** is a normal platform driver. It registers a `wall_clock` provider
  when it probes; timekeeper starts with a zero wall offset and applies the
  RTC's time once the provider is registered.
- **clocksource/timebase**: the RISC-V time CSR is registered as a
  `clocksource` before initcalls; the selected source feeds timekeeper and
  timer tick programming.  Early memory/CPU discovery stays before initcalls
  because the rest of the boot path depends on it.
- **UART/console** is registered from the platform probe; messages produced
  before that are buffered by printk.

### Timekeeping

Time is layered as `clocksource` → `timekeeper` → `wall_clock`:

- `clocksource` provides the monotonic cycle counter selected at boot.
- `timekeeper` owns monotonic nanoseconds, the wall-clock offset and jiffies.
- `wall_clock` is registered by a normal RTC platform driver.

The boot hart's timer tick calls `timekeeper_tick()`, which advances jiffies,
runs registered tick handlers (for example TCP retransmission timers), and
wakes sleepers. Secondary harts only reprogram their timer.

`CLOCK_REALTIME` is monotonic time plus the wall offset; wall-clock changes do
not affect `CLOCK_MONOTONIC` or `CLOCK_BOOTTIME`. `clock_getres()` reports the
clocksource resolution for monotonic clocks and the tick resolution for CPU
clocks.  Since BRK has no suspend support yet, `CLOCK_BOOTTIME` is currently
equivalent to `CLOCK_MONOTONIC`.

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.
