# BRK 内核设计文档

本目录保存 BRK 内核的整体架构、启动流程和各子系统的设计说明。文档以当前 `src/` 代码为准，描述的是“已经实现的行为”，而不是未来规划。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | 整体架构、代码分层、执行模型、地址空间、子系统关系、跨子系统规则 |
| [boot.md](boot.md) | 从 `_start` 到用户态 init 的完整启动与初始化流程，SMP 启动、陷阱入口、上下文切换 |
| [process.md](process.md) | 任务、调度器、静态 init task、fork/exec/exit/wait、信号、系统调用、同步原语 |
| [memory.md](memory.md) | memblock、伙伴页分配器、slab/kmalloc/vmalloc、用户虚拟内存、mmap/COW、页缓存 |
| [filesystem.md](filesystem.md) | VFS 对象模型、挂载、路径查找、缓存、writeback、pipe、tmpfs/devtmpfs/procfs/brkfs |
| [drivers.md](drivers.md) | 设备模型、设备号、块层、virtio、TTY/串口、RTC、设备树扫描 |
| [networking.md](networking.md) | sk_buff、netdevice、ARP/IP/ICMP/UDP/TCP、socket 系统调用、virtio-net |
| [irq_time.md](irq_time.md) | 中断描述符/IRQ domain/PLIC、时钟与时间保持、printk/console/klogd、panic |
| [kernel_log.md](kernel_log.md) | 内核日志环形缓冲区、writer/reader 协议、console/klogd 消费模型 |

建议阅读顺序：

1. `architecture.md`：先建立整体地图。
2. `boot.md`：理解内核如何从固件进入 C 代码并启动第一个任务。
3. `process.md` / `memory.md`：理解任务和地址空间这两个核心抽象。
4. `filesystem.md` / `drivers.md` / `networking.md` / `irq_time.md`：按需深入子系统。

## 一句话架构

BRK 是一个面向 RISC-V 64 位的宏内核：

```text
用户程序
   │  ecall / trap
   ▼
系统调用 + 陷阱处理
   │
   ├── 进程/调度器 ── 同步原语 ── 信号
   ├── 虚拟内存 / 页缓存
   ├── VFS ── tmpfs / devtmpfs / procfs / brkfs
   ├── 块层 ── virtio-blk
   ├── 网络协议栈 ── virtio-net
   ├── 设备模型 ── 字符设备 / TTY / 平台设备
   └── IRQ / 时钟 / printk
```

## 关键设计特征

- **单体内核**：所有子系统运行在同一内核地址空间中，通过函数调用和明确的锁规则协作。
- **RISC-V Sv39**：内核高地址映射，用户空间独立页表；用户页表复制内核 PGD 项。
- **SMP**：每 CPU 一个 `struct cpu`、运行队列和 idle task；通过 IPI 唤醒远端 CPU。
- **静态 boot/init task**：每个 CPU 的启动上下文本身是一个静态任务；CPU0 的 init task 完成文件系统初始化并成为孤儿进程回收者。
- **FIFO 调度**：没有优先级、调度类和负载均衡器，只有每 CPU 运行队列、时间片和 idle fallback。
- **显式锁与引用计数**：spinlock、sleeplock、refcnt 是大部分对象生命周期的基础。
- **分层但不过度抽象**：VFS、设备模型、块层、网络栈都有清晰接口，但实现保持小型化和可读性。

## 文档约定

- `src/...` 路径均相对于仓库根目录。
- “当前实现”表示代码中已经存在的行为；如果某功能只有接口没有完整实现，会在对应文档的“限制”或“未实现”小节说明。
- 文档中的代码片段用于说明结构，不保证与源码逐字一致；以源码为准。
