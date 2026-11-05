# BRK 整体架构

本文描述 BRK 的总体设计、代码分层、执行模型、地址空间和子系统之间的边界。具体的启动步骤见 [boot.md](boot.md)，任务/内存/文件系统等细节见各自的专题文档。

## 1. 设计目标与风格

BRK 是一个面向 RISC-V 64 位（rv64gc）的宏内核，主要目标不是兼容完整 Linux ABI，而是用尽量清晰的小型实现覆盖一个操作系统内核的核心机制：

- 启动与早期控制台
- 物理/虚拟内存管理
- 任务、调度和上下文切换
- 用户态、系统调用、陷阱和信号
- 虚拟文件系统与若干真实文件系统
- 块设备、字符设备、TTY、virtio
- 中断控制器、时钟、SMP
- IPv4 网络栈和 socket
- printk/console/klogd 日志

代码风格强调：

- 数据结构直接、布局可见；
- 关键路径使用显式锁和引用计数；
- 架构相关代码集中在 `src/arch/riscv/`；
- 通用头文件放在 `src/include/brk/`，用户 ABI 放在 `src/include/uapi/`；
- 不使用 C 运行时，内核自带的 `src/lib/` 提供字符串、格式化、排序、哈希等基础函数。

## 2. 构建与运行目标

| 项目 | 值 |
| --- | --- |
| 架构 | RISC-V 64 位，`rv64gc` / `lp64d` |
| 代码模型 | `medany` |
| 内核链接地址 | `KERNEL_LINK_ADDR = 0xffffffff80000000` |
| 内核加载地址 | `KERNEL_LOAD_ADDR = 0x80200000` |
| 页大小 | 4 KiB |
| 分页模式 | Sv39 |
| 最大 CPU 数 | `MAX_CPUS = 8` |
| 默认 QEMU 机器 | `virt`，OpenSBI `-bios default` |
| 构建入口 | 顶层 `Makefile` |

内核镜像同时包含高地址链接视图和低地址加载视图。`head.S` 在物理地址运行，计算 `kernel_image_offset = KERNEL_LINK_ADDR - runtime _skernel`，建立早期页表后跳到高地址继续执行。这个偏移保存在 `boot_info.kernel_image_offset` 中，之后用 `symbol_phys()` 把内核符号转换为物理地址。

完整构建命令和 QEMU 参数见顶层 `README.md` 与 `Makefile`。

## 3. 源码目录与分层

```text
src/
├── arch/riscv/                 RISC-V 架构相关代码
│   ├── include/arch/           架构头文件：CSR、页表、陷阱、上下文、SBI
│   ├── kernel/                 入口、CPU、陷阱、定时器、RTC、上下文切换
│   └── mm/                     早期页表、最终页表、用户页表操作
├── include/
│   ├── brk/                    通用内核接口和类型
│   ├── brk/asm/                与架构共享的常量（MAX_CPUS、启动栈大小）
│   ├── brk/irq/                IRQ 核心接口
│   ├── brk/net/                网络协议栈接口
│   ├── uapi/                   用户态 ABI（syscall、errno、signal、stat 等）
│   └── freestanding/           freestanding 环境补充头文件
├── kernel/
│   ├── main.c                  start_kernel 和 initcall 调度
│   ├── cpu.c                   每 CPU 数据、preempt_count、current task
│   ├── proc/                   任务、调度器、进程相关系统调用
│   ├── sync/                   spinlock、sleeplock
│   ├── irq/                    IRQ descriptor/domain/chip 框架
│   ├── time/                   clocksource、timekeeper、时间系统调用
│   ├── log/                    printk、console、klogd
│   ├── signal.c                信号实现
│   ├── syscall.c               系统调用表与分发
│   └── panic.c                 panic 路径
├── mm/                         物理页、slab、kmalloc、vmalloc、uvm、mmap、页缓存
├── fs/                         VFS、exec、pipe、select、writeback、文件系统
│   ├── tmpfs/                  内存文件系统
│   ├── devtmpfs.c              设备节点自动管理
│   ├── procfs/                 proc 文件系统
│   └── brkfs/                  BRK 自有磁盘文件系统
├── driver/
│   ├── base/                   设备模型、设备号、块层、bio、gendisk、chrdev
│   ├── block/                  virtio-blk
│   ├── net/                    virtio-net
│   ├── char/                   ns16550a UART
│   ├── tty/                    TTY core、n_tty 行规程
│   ├── virtio/                 virtio MMIO 传输和 virtqueue
│   ├── dt/                     设备树扫描和平台设备注册
│   ├── irqchip/                SiFive PLIC
│   └── rtc/                    Goldfish RTC
├── net/                        sk_buff、netdevice、ARP/IP/ICMP/UDP/TCP/socket
├── lib/                        字符串、printf、qsort、hash、bitmap、refcnt、rand
└── ...
```

依赖方向大致为：

```text
arch ──► 通用 CPU/陷阱接口
lib  ◄── 所有子系统
mm   ◄── kernel/fs/driver/net
kernel/proc ◄── 调度、信号、系统调用
fs   ◄── VFS、具体文件系统
driver ◄── 设备模型、块/字符/网络/TTY
net  ◄── 协议栈、socket、netdevice
```

模块之间仍然有大量直接调用，这是单体内核的预期行为；关键是保持对象所有权和锁规则清晰。

## 4. 执行模型

### 4.1 特权级

BRK 运行在 RISC-V S-mode，OpenSBI 提供 M-mode 服务：

- 内核通过 SBI 完成定时器设置、IPI、hart 启动和早期控制台输出；
- 用户程序运行在 U-mode；
- 用户态通过 `ecall` 进入内核的 `trap_entry`；
- 内核态异常/中断也进入同一个 `trap_entry`，由 `sstatus.SPP` 区分来源。

### 4.2 每 CPU 状态

每 CPU 有一个 `struct cpu`：

```c
struct cpu {
    struct task *handoff;
    struct task *current;
    struct task *idle;
    cpuid_t id;
    int preempt_count;
    bool initialized;
    bool online;
    struct list_head run_queue;
    spinlock_t run_queue_lock;
    unsigned int nr_running;
};
```

`this_cpu()` 通过 RISC-V `tp` 寄存器取得当前 CPU 的 `struct cpu *`。在内核态中，`tp` 始终是每 CPU 指针；用户态可能使用自己的 `tp`，用户陷阱入口会从陷阱帧的 `kernel_tp` 字段恢复内核 `tp`。

### 4.3 抢占与中断

- `preempt_count` 是每 CPU 计数，不是每任务计数。
- `spin_lock()` / `spin_unlock()` 会调整 `preempt_count`；`spin_lock_irqsave()` 不调整，因为中断已经关闭。
- 中断/陷阱处理中只有在 `preempt_count() == 0` 时才允许调用 `task_yield()`。
- 调度器切换上下文时要求中断关闭且 `preempt_count == 0`。
- `raw_spin_lock()` 只用于调度器内部跨上下文切换持有的 handoff 锁。

### 4.4 SMP 模型

- 每个 CPU 有独立运行队列和 idle task。
- 新任务/唤醒任务通过 `task_select_cpu()` 选择负载最低的在线 CPU。
- 远端 CPU 通过 SBI IPI 唤醒；如果 IPI 不可用，定时器 tick 最终会检查运行队列。
- 每个 CPU 的启动上下文是一个静态 `init_task`；CPU0 的 init task 完成文件系统初始化后成为孤儿进程回收者。
- 二级 CPU 在 `secondary_entry()` 中初始化自己的陷阱、定时器和 PLIC 上下文，然后进入 `task_scheduler()`。

## 5. 核心对象模型

### 5.1 任务

`struct task` 是调度、资源和身份的核心对象。几个重要字段组：

| 字段 | 作用 |
| --- | --- |
| `list` | 全局任务链表 |
| `queue` | 运行队列或睡眠队列节点，同一时刻只属于一个队列 |
| `parent/children/child` | 父子关系和 wait 语义 |
| `lock` | 调度器跨上下文切换持有的任务锁 |
| `kernel_sp` | 保存的上下文栈指针 |
| `state` | NEW/RUNNING/SLEEPING/ZOMBIE |
| `mm` | 用户地址空间 |
| `fdtable` | 打开文件描述符表 |
| `fsinfo` | root/cwd 等文件系统上下文 |
| `sigactions` | 信号动作表和 pending/blocked 状态 |
| `frame` | 用户陷阱帧固定位置 |
| `kstack_base/top` | 内核栈范围 |
| `oncpu` | 当前运行的 CPU，-1 表示不在 CPU 上 |
| `cpu` | 最近分配/选择的 CPU |
| `is_init_task` | 静态启动任务标记，禁止销毁 |

### 5.2 地址空间

每个用户任务拥有一个 `struct uvm_space`：

- `pgd`：用户页表根；
- `seg`：普通 VMA 链表；
- `stack`：用户栈区域；
- `heap`：brk 堆区域；
- `brk`、`mmap_top`：地址分配游标；
- `refcnt`：地址空间引用计数。

内核栈、内核页表、vmalloc、vmemmap 等属于全局内核地址空间，不放在 `uvm_space` 中。

### 5.3 文件系统对象

VFS 的核心对象：

```text
fs_driver ──► fs_super_block ──► fs_inode ──► fs_dentry ──► fs_file
                                      ▲             │
                                      └── inode hash│
                                                    └── mount / path
```

- `fs_dentry` 负责名字到 inode 的缓存；
- `fs_path` = `{ mount, dentry }`；
- `fs_file` 表示一次打开的文件描述；
- `fsinfo` 保存任务的 root/cwd；
- `fdtable` 保存文件描述符到 `fs_file` 的映射。

### 5.4 设备与网络对象

- `struct device` / `struct device_driver` / `struct bus_type` 构成设备模型。
- `struct block_dev` / `struct gendisk` / `struct request_queue` / `struct bio` 构成块层。
- `struct net_device` / `struct sk_buff` / `struct sock` / `struct socket` 构成网络栈。
- `struct irq_desc` / `struct irq_domain` / `struct irq_chip` 构成 IRQ 框架。

## 6. 地址空间布局

| 区域 | 起始地址 | 大小/说明 |
| --- | --- | --- |
| 用户空间 | `0` | 最大到 `USER_SPACE_SIZE_MAX = 0x3ffffff000` |
| 线性映射 | `PAGE_OFFSET = 0xffffffc000000000` | 128 GiB，物理内存直接映射 |
| vmalloc | `VMALLOC_START = 0xffffffe000000000` | 64 GiB |
| vmemmap | `VMEMMAP_START = 0xfffffff000000000` | 4 GiB，`struct page` 数组 |
| 内核镜像 | `KERNEL_LINK_ADDR = 0xffffffff80000000` | 代码/数据/BSS/启动栈 |

注意：

- `phys_to_virt()` / `virt_to_phys()` 只适用于线性映射区域；
- 内核镜像符号不是线性映射别名，物理地址要用 `symbol_phys()`；
- 用户页表创建时复制内核 PGD 项，因此用户态运行时内核高地址映射仍然可见；
- 之后动态添加到内核页表的新 vmalloc/ioremap 区域不会自动同步到已经创建的用户页表。

## 7. 启动与运行时阶段

整体阶段可以概括为：

```text
OpenSBI
  │  a0 = hart id, a1 = DTB
  ▼
_start（物理地址）
  ├─ 清 BSS
  ├─ 设置 per-hart 启动栈
  ├─ 安装早期 trap vector
  ├─ 建立早期 Sv39 页表
  └─ 跳到高地址 primary_entry
        │
        ▼
start_kernel
  ├─ arch_init：最终页表、DTB 扫描
  ├─ mm_core_init：伙伴分配器、slab、kmalloc、vmalloc
  ├─ clocksource/RTC/timekeeper/timer
  ├─ trap_init
  ├─ do_initcalls
  ├─ local_irq_enable
  └─ task_scheduler
        │
        ├─ CPU0：静态 init_task → fs_init → 创建 PID1 → 成为 reaper
        └─ 其他 CPU：静态 boot task → 切换到动态 idle
              │
              ▼
          用户态 init / shell
```

详细的每一步见 [boot.md](boot.md)。

## 8. 子系统协作关系

```text
                    ┌──────────────┐
                    │   用户程序    │
                    └──────┬───────┘
                           │ ecall
                    ┌──────▼───────┐
                    │ trap/syscall │
                    └──┬───┬───┬───┘
                       │   │   │
          ┌────────────▼┐ ┌▼───▼────────┐ ┌──────────────┐
          │ 进程/调度器  │ │ VFS/文件系统 │ │ socket/网络   │
          └──┬───────┬──┘ └──┬───────┬──┘ └──┬───────┬───┘
             │       │       │       │       │       │
        ┌────▼───┐ ┌─▼───────▼─┐ ┌───▼────┐ ┌▼───────▼───┐
        │ 信号    │ │ 页缓存/VM │ │ 块层    │ │ netdevice  │
        └────────┘ └─────┬─────┘ └───┬────┘ └─────┬──────┘
                         │           │            │
                    ┌────▼───────────▼────────────▼────┐
                    │       设备模型 / 驱动 / IRQ       │
                    └───────────────┬──────────────────┘
                                    │
                    ┌───────────────▼──────────────────┐
                    │ 物理页分配器 / slab / vmalloc      │
                    └──────────────────────────────────┘
```

## 9. 跨子系统规则

### 9.1 锁的分类

| 类型 | 是否可睡眠 | 是否关闭中断 | 典型用途 |
| --- | --- | --- | --- |
| `spin_lock` | 否 | 否 | 短临界区，自动 preempt_disable |
| `spin_lock_irqsave` | 否 | 是 | 可能被中断访问的数据；可跨 `task_sleep` 释放/重获 |
| `raw_spin_lock` | 否 | 否 | 调度器跨上下文切换持有的 handoff 锁 |
| `sleeplock_t` | 是 | 内部 spinlock 短暂关闭 | 文件系统 I/O、可阻塞的互斥 |

关键规则：

- 中断处理中不能睡眠；
- 持有普通 `spin_lock` 时不能睡眠，也不能被抢占；
- `spin_lock_irqsave` 不改变 `preempt_count`，因此可以在它保护的临界区内调用 `task_sleep`，但释放/重获由调用者负责；
- 调度器在 IRQ 关闭、`preempt_count == 0` 时切换任务；
- `task->lock` 在上下文切换期间由调度器持有，由下一个任务在 `task_sched_resume()` / `complete_task_switch()` 中释放。

### 9.2 引用计数

常用对象：

- `struct task`：生命周期由父进程 wait/销毁和调度器 handoff 规则保护；
- `uvm_space`、`fs_inode`、`fs_dentry`、`fs_file`、`fs_mount_state`、`fdtable`、`fsinfo`、`sigaction_table`；
- 页缓存 `cached_page`；
- `page` 的 `refcnt` 用于 COW 共享页。

一般规则：

- 查找/取得对象时增加引用；
- 把对象放入缓存或队列时，缓存/队列持有一个引用；
- 最后一个引用释放时执行析构；
- 析构可能睡眠的对象（如 inode eviction、文件系统路径释放）不能在 spinlock 内完成。

### 9.3 initcall 顺序

初始化函数通过：

```c
core_initcall(fn);      /* .initcall0 */
postcore_initcall(fn);  /* .initcall1 */
subsys_initcall(fn);    /* .initcall2 */
device_initcall(fn);    /* .initcall3 */
```

放入链接器保留的 section。`do_initcalls()` 按链接顺序执行，没有拓扑排序，也不能返回错误。因此依赖关系必须靠“更早的 level”保证。

当前没有 `device_initcall` 用户；用户 init 的创建已经移到静态 init task 中。

## 10. 当前范围与已知限制

BRK 的设计目标是逐步覆盖核心机制，不是完整 Linux 兼容实现。阅读代码和文档时应注意：

- 调度器是 FIFO、无优先级、无调度类、无周期性负载均衡；
- `clone`/线程组完整语义尚未实现，`struct task` 中已有部分 thread-group 字段；
- 信号是位图而不是队列，没有 `siginfo`、`SA_RESTART`、作业控制停止/继续；
- `task_wait()` 忽略 `options`/`rusage`，没有 `WNOHANG` 和信号中断检查；
- 用户态直接解引用用户指针的场景仍然存在，不是所有路径都经过 `copy_from_user`；
- 文件系统没有统一 journal，brkfs 依赖 CRC、DIRTY/CLEAN 状态和 orphan 清理；
- 动态添加到内核页表的映射不会同步到已有用户页表；
- 网络栈没有分片重组、NAPI/softirq、完整 TCP 状态机健壮性；
- 设备模型/驱动注册没有完整的引用计数和热插拔等待机制。

这些限制在对应专题文档中会进一步展开。
