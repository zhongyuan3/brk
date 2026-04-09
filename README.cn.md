[中文](README.cn.md) | [English](README.md)

# BRK

### **B**arely **R**unning **K**ernel

> _"Yes, it's a BRK. But it's MY BRK."_

![Build Status](https://img.shields.io/badge/build-probably_passing-yellow)
![Language](https://img.shields.io/badge/language-C-blue)
![Arch](https://img.shields.io/badge/target-RISCV--64-orange)
![Mood](https://img.shields.io/badge/mood-panic%20but_happy-red)

## 这是个什么玩意儿？

BRK 是一个完全出于个人兴趣、用纯 C 语言编写的宏内核。

为什么叫 "Barely Running Kernel"（勉强运行的内核）？因为在写内核的大部分时间里，它要么卡在 QEMU 的无限死循环里，要么正在遭遇 Kernel Panic。但每当它成功引导、打印出第一个 `$` 提示符的那一秒钟，它确实是“跑起来了”。

目前它主要面向 **RISC-V (rv64)** 架构，但我的最终目标是让它成为一个架构无关的内核——毕竟，优秀的代码不应该被特定的指令集束缚。

## 灵感与致谢

我并不打算重新发明轮子，BRK 的诞生完全建立在巨人的肩膀上。如果你熟悉以下两个项目，你会在 BRK 的代码里看到很多它们的影子：

- 🐮 **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)**：BRK 的**启蒙导师**。在项目初期，xv6 为我指明了如何在 RISC-V 上进行机器模式切换、如何设置页表、以及如何处理基本的 Trap。BRK 的早期内存布局和启动流程深受其影响。
- 🐧 **[Linux Kernel](https://github.com/torvalds/linux)**：BRK 的**终极蓝图**。随着项目推进，我开始脱离 xv6 的简单框架，转而参考 Linux 的设计哲学。从宏内核的子系统划分、链表和红黑树的实现、到 slab 内存分配器的思路，我都试图在理解 Linux 的基础上，用我自己的方式在 BRK 中重新实现一遍。

**简单来说：xv6 教我如何起步，Linux 教我如何走向远方。**

## 当前状态 (The "Barely" Part)

目前 BRK 刚刚脱离了“只能打印字符”的婴儿期，可能包括但不限于以下这些“勉强能跑”的特性：

- [x] 基于 RISC-V SBI 的早期控制台输出
- [x] 物理内存探测与基本的段/页式内存管理
- [x] 极简的内核堆分配器 (大概比 `brk()` 系统调用高级一点)
- [x] 时钟中断与基础的 Trap 上下文保存/恢复
- [ ] 虚拟文件系统 (VFS) —— _正在脑海中设计_
- [x] 用户态与系统调用
- [ ] 多核 (SMP) 支持 —— _目前还是单核孤狼_

## 未来计划

1.  **架构解耦**：将 RISC-V 相关的汇编和底层代码抽离，为未来移植到 x86 或 ARM 留好接口。
2.  **写一个真正的文件系统**：不再把数据硬编码在内存里。
3.  **让它不仅仅 "Barely Running"**：目标是 "Stably Running"（虽然名字可能不改了）。

## 构建与运行

如果你实在好奇一个“勉强运行的内核”长什么样，可以尝试在 RISC-V 环境下跑一下。

**依赖：**

- `riscv64-unknown-elf-toolchain` (或类似的 RISC-V 交叉编译工具链)
- `QEMU` (系统模式，支持 `riscv64`)

**跑一下试试：**
