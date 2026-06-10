[中文](README.cn.md) | [English](README.md)

# BRK

### **B**arely **R**unning **K**ernel

> _"Yes, it's a BRK. But it's MY BRK."_

![Build Status](https://img.shields.io/badge/build-probably_passing-yellow)
![Language](https://img.shields.io/badge/language-C-blue)
![Arch](https://img.shields.io/badge/target-RISCV--64-orange)
![Mood](https://img.shields.io/badge/mood-panic%20but_happy-red)

## What the heck is this?

BRK is a monolithic kernel written entirely in C, driven purely by personal curiosity (and maybe a slight touch of madness).

Why call it the "Barely Running Kernel"? Because for 90% of the development cycle, it is either stuck in an infinite loop inside QEMU or throwing a spectacular Kernel Panic. But for that glorious 10% of the time when it successfully boots, sets up the page tables, and drops to a shell prompt... it is technically running.

While it currently targets the **RISC-V (rv64)** architecture, the underlying code is being written with abstraction in mind. Good kernel code shouldn't be held hostage by a specific instruction set.

## Inspiration & Origins

I'm not trying to reinvent the wheel here. BRK stands firmly on the shoulders of giants. If you are familiar with the following projects, you will immediately recognize their DNA in my codebase:

- 🐮 **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv) — The Bootstrapper:** In the early days, xv6 was my absolute guide. It taught me how to survive the bare-metal RISC-V environment: machine-mode transitions, initial page table setup, and basic trap handling. BRK's early memory layout and boot flow owe a lot to MIT's legendary teaching OS.
- 🐧 **[Linux Kernel](https://github.com/torvalds/linux) — The North Star:** As BRK began to outgrow its xv6 training wheels, I started looking towards the ultimate monolithic kernel for inspiration. From subsystem boundaries and robust linked list macros to the philosophy behind the Slab allocator, I am trying to understand Linux's design and reimplement those concepts in my own, much simpler way.

**In short: xv6 taught me how to start; Linux taught me how far I need to go.**

## Current Status (The "Barely" Part)

BRK has recently graduated from just printing characters to the console. It currently features a mix of "things that actually work" and "things that are held together by duct tape and prayers":

- [x] Early console output via RISC-V SBI
- [x] Physical memory probing and basic page memory management
- [x] A rudimentary kernel heap allocator (slightly better than a basic `sbrk()`)
- [x] Timer interrupts and basic Trap context save/restore
- [x] Virtual File System (VFS)
- [x] User mode & System Calls
- [ ] Multi-core (SMP) support — _Still a lonely, single-core beast_

## The Roadmap

1.  **Architecture Decoupling:** Abstracting RISC-V specific assembly and MMU code to prepare for future x86 or ARM ports.
2.  **A Real File System:** Stopping the practice of hardcoding everything into memory.
3.  **Stably Running:** The ultimate goal is to drop the "Barely" from the name (though I'll probably keep the acronym).

## Build & Run (At Your Own Risk)

If you are morbidly curious to see what a "barely running kernel" looks like, you can try spinning it up in a RISC-V environment.

**Prerequisites:**

- `riscv64-unknown-elf-toolchain` (or any RISC-V cross-compiler)
- `QEMU` (System mode, `qemu-system-riscv64`)

**Try it out:**
