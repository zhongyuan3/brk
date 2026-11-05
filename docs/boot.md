# 启动与初始化流程

本文描述 BRK 从 OpenSBI 移交控制权开始，到 CPU0 进入用户态 init、二级 CPU 上线并进入 idle 的完整流程。相关源码：

- `src/arch/riscv/kernel/head.S`
- `src/arch/riscv/kernel/init.c`
- `src/arch/riscv/kernel/cpu.c`
- `src/arch/riscv/kernel/trap_entry.S`
- `src/arch/riscv/kernel/trap.c`
- `src/arch/riscv/kernel/timer.c`
- `src/arch/riscv/kernel/switch_context.S`
- `src/arch/riscv/mm/init.c`
- `src/arch/riscv/mm/page_table.c`
- `src/kernel/main.c`
- `src/kernel/cpu.c`
- `src/kernel/proc/sched.c`
- `src/kernel/proc/task.c`
- `src/include/brk/initcall.h`

## 1. 启动总览

```text
OpenSBI
  │ a0 = hart id
  │ a1 = DTB 物理地址
  ▼
_start / _secondary_start          (head.S，物理地址运行)
  ├─ 清 BSS（仅 boot hart）
  ├─ 计算 kernel_image_offset
  ├─ 设置每 hart 启动栈
  ├─ 安装早期 trap_entry
  ├─ 建立早期 Sv39 页表并开启分页
  └─ 跳到高地址 primary_entry / secondary_entry
        │
        ├─────────────── primary_entry ───────────────┐
        │                                             │
        │ cpu_init, setup_trap_entry, dt_set_fdt      │
        │ boot_info 初始化                             │
        │ start_kernel                                │
        │   ├─ arch_init                              │
        │   ├─ mm_core_init                           │
        │   ├─ clocksource / RTC / timekeeper / timer │
        │   ├─ trap_init                              │
        │   ├─ do_initcalls                           │
        │   ├─ irq_init_hart                          │
        │   ├─ local_irq_enable                       │
        │   └─ task_scheduler                         │
        │                                             │
        │ CPU0: 静态 init_task → fs_init → PID1       │
        │        → orphan reaper → idle fallback       │
        │                                             │
        └─────────────── secondary_entry ─────────────┘
              │ cpu_init, setup_trap_entry
              │ load_final_page_table
              │ timer_init_hart, trap_init, irq_init_hart
              │ local_irq_enable
              └─ task_scheduler
                    └─ 静态 boot task → 动态 idle
```

关键分界点：

1. **物理地址阶段**：`head.S` 在低地址执行，建立最小页表。
2. **高地址内核阶段**：跳到 `KERNEL_LINK_ADDR` 后运行正常 C 代码。
3. **单核初始化阶段**：boot hart 依次初始化内存、时间、IRQ 和驱动。
4. **任务阶段**：`task_scheduler()` 把当前启动上下文变成静态 `init_task`。
5. **文件系统/用户阶段**：CPU0 的 `init_task` 运行 `fs_init()`，创建 PID 1，再变成回收者。
6. **SMP 阶段**：PID 1 在 exec 成功后调用 `bring_up_secondaries()`，二级 CPU 从 `_secondary_start` 进入内核。

## 2. 物理入口与早期页表

### 2.1 `_start`

`src/arch/riscv/kernel/head.S` 的 `_start` 是 boot hart 的入口，运行在 OpenSBI 移交的物理地址（默认 `0x80200000`）。

```asm
_start:
    mv s2, a0                  # hart id
    mv s3, a1                  # DTB 物理地址
    csrw sscratch, zero
    compute_kernel_image_offset
    clear BSS
    setup_per_hart_stack hart_id
    call setup_trap_entry_early
    call setup_vm_early
    jump_to_kernel_virt primary_entry
```

步骤解释：

1. **保存 hart id 和 DTB**：后续 C 入口需要。
2. **清零 `sscratch`**：建立“内核态 `sscratch == 0`”不变量。
3. **计算镜像偏移**：
   ```text
   kernel_image_offset = KERNEL_LINK_ADDR - runtime _skernel
   ```
   该值存入 `s4`，之后传给 `primary_entry`。
4. **清 BSS**：从 `_sbss` 到 `_ebss`，包括每 hart 启动栈、早期页目录保留页、`kernel_pgdir` 以及普通 BSS。
5. **设置启动栈**：
   - 每个 hart 在 `init_stack_rsvd_pages` 中有 16 KiB（`INIT_STACK_PAGE_ORDER = 2`）；
   - 栈顶为 `init_stack_rsvd_pages + (hart_id + 1) * 16 KiB`；
   - 初始 `sp = 栈顶 - TRAP_FRAME_SIZE`，顶部 304 字节预留给 init task 的陷阱帧。
6. **早期 trap vector**：`setup_trap_entry_early()` 把 `stvec` 设为物理地址的 `trap_entry`，并保持 `in_early = true`；早期陷阱直接通过 SBI 输出后停机。
7. **早期页表**：`setup_vm_early()` 建立 Sv39 页表并写 `satp`。
8. **跳到高地址**：`sp` 和入口地址都加上 `kernel_image_offset`，然后 `jr` 到链接地址。

### 2.2 `_secondary_start`

二级 hart 的入口类似，但有几处不同：

- 不清 BSS：boot hart 已经清过；
- 不接收 DTB 参数；
- 只加载已经建立好的早期页表；
- 仍然使用自己的 per-hart 启动栈和顶部的陷阱帧保留区；
- 跳到高地址的 `secondary_entry`。

### 2.3 早期页表

`__setup_vm_early()` 位于 `src/arch/riscv/mm/init.c`，使用 BSS 中预留的早期页目录页：

- 建立内核镜像的物理地址恒等映射；
- 建立高地址 `KERNEL_LINK_ADDR` 到物理镜像的映射；
- 必要时映射 DTB；
- 页表页从 `early_pgdir_rsvd_pages` 顺序分配；
- 允许使用 1 GiB / 2 MiB 大页，剩余部分用 4 KiB 页。

`make_satp()` 使用 Sv39 模式 8。早期页表只用于启动过渡，之后切换到最终内核页表。

## 3. `primary_entry` 与 `start_kernel`

`primary_entry()`：

```c
cpu_init(hart_id);
setup_trap_entry();

dt_set_fdt(dtb_pa);
boot_info.boot_hart_id = hart_id;
boot_info.dtb_pa = dtb_pa;
boot_info.kernel_image_offset = kernel_image_offset;

start_kernel();
```

`cpu_init()` 设置：

```text
cpus[hart_id].id = hart_id
cpus[hart_id].preempt_count = 0
cpus[hart_id].initialized = true
tp = &cpus[hart_id]
```

`setup_trap_entry()` 把 `stvec` 设为高地址的 `trap_entry`，并清除 `in_early`。

`start_kernel()` 的当前顺序：

```c
void start_kernel(void)
{
    arch_init();
    mm_core_init();

    clocksource_init();
    rtc_init();
    timekeeper_init();
    timer_init();

    trap_init();

    do_initcalls();

    irq_init_hart(this_cpuid());

    local_irq_enable();

    task_scheduler();
}
```

### 3.1 `arch_init()`

- `paging_init()`：
  - `phys_mem_init()`：从设备树收集 `/memory` 和 `/reserved-memory`，保留内核镜像和 DTB，设置 `phys_offset`；
  - `final_pgtable_init()`：构建 `kernel_pgdir`，映射 text/rodata/data/bss/free memory/DTB；
  - `load_final_page_table()`：切换到最终页表；
  - `dt_set_fdt(phys_to_virt(dtb_pa))`：把 FDT 指针切到线性映射地址；
  - `vmemmap_init()`：为所有 PFN 建立 `struct page` 数组并映射到 `VMEMMAP_START`。
- `dt_init_scan_cpu()`：扫描 `/cpus`，统计 `nr_harts`。

### 3.2 `mm_core_init()`

```c
page_alloc_init();
memblock_free_all();
kmalloc_init();
vmalloc_init();
memblock_dump_all();
```

- `page_alloc_init()`：初始化伙伴分配器和每 CPU 页缓存；
- `memblock_free_all()`：把未保留内存移交给页分配器；
- `kmalloc_init()`：创建 8 到 4096 字节的 slab 缓存；
- `vmalloc_init()`：创建 vmalloc 区域链表和 vmalloc 元数据缓存。

### 3.3 时间初始化

- `clocksource_init()`：从设备树读取 `timebase-frequency`，失败时使用 10 MHz；
- `rtc_init()`：查找 Goldfish RTC 并 `ioremap`；
- `timekeeper_init()`：记录单调时间基准和 RTC 墙钟偏移；
- `timer_init()`：计算 1 ms tick 间隔并编程第一次定时器。

### 3.4 `trap_init()`

使能 `sie` 中的：

- `STIE`：supervisor timer interrupt；
- `SEIE`：supervisor external interrupt；
- `SSIE`：supervisor software interrupt（IPI）。

注意此时还没有设置 `sstatus.SIE`，全局中断在 `start_kernel()` 末尾才打开。

### 3.5 `do_initcalls()`

初始化函数通过链接器 section 排序：

| 宏 | Section | 执行顺序 | 当前用途 |
| --- | --- | --- | --- |
| `core_initcall` | `.initcall0` | 最先 | CPU 运行队列、IRQ descriptor、任务/内存缓存、设备总线、VFS 缓存、网络核心 |
| `postcore_initcall` | `.initcall1` | 其次 | IRQ chip 探测、IRQ 自测 |
| `subsys_initcall` | `.initcall2` | 再次 | 串口/virtio 设备扫描、块驱动、网络驱动、TTY 驱动 |
| `device_initcall` | `.initcall3` | 最后 | 当前没有用户 |

当前构建中的顺序（同一 level 内按链接顺序）：

```text
core:
  cpu_sched_init
  irq_init
  task_cache_init
  uvm_space_cache_init
  page_cache_init
  blkdev_registry_init
  chrdev_registry_init
  platform_bus_init
  virtio_bus_init
  plic_driver_register
  fs_dentry_cache_init
  fs_file_cache_init
  fs_inode_cache_init
  net_init

postcore:
  irqchip_init
  irq_selftest

subsys:
  dt_init_scan_serial
  virtio_blk_init
  virtio_net_init
  ns16550a_driver_init
```

重要限制：

- initcall 返回 `void`，不能传播错误；
- 不能睡眠、不能依赖 `current_task()`；
- 依赖关系靠 level 和链接顺序保证；
- 没有释放 init section。

### 3.6 `task_scheduler()`

`start_kernel()` 在中断使能后调用 `task_scheduler()`。这是从“普通 C 启动上下文”进入“任务调度世界”的边界。

## 4. 静态 init task 与每 CPU 启动上下文

### 4.1 每 CPU 的静态任务

`src/kernel/proc/sched.c`：

```c
static struct task init_tasks[MAX_CPUS];
```

每个 CPU 的启动栈（`init_stack_rsvd_pages` 中对应 hart 的那一块）就是该 CPU 静态 init task 的内核栈。`init_task_setup()`：

- 初始化链表、队列、父子链表、锁；
- `state = RUNNING`，`oncpu = -1`，`cpu = cpuid`；
- `is_init_task = true`，PID/TGID/PGID/SID 都为 0；
- 设置 root 身份、`umask = 022`、完整 cap；
- `kstack_top = arch_boot_stack_top(cpuid)`；
- `kstack_base = kstack_top - KSTACK_SIZE`；
- `frame = kstack_top - TRAP_FRAME_SIZE`；
- 清零陷阱帧并设置 `kernel_tp = &cpus[cpuid]`；
- 仅 boot hart 分配 `fsinfo`、`fdtable` 并初始化信号表。

启动栈和普通内核栈大小必须一致：

```c
static_assert(KSTACK_SIZE == (PAGE_SIZE << INIT_STACK_PAGE_ORDER), ...);
```

### 4.2 `task_scheduler()` 公共流程

```c
local_irq_disable();

init_task_setup(init, cpu->id);
idle = task_create_idle(cpu->id);

cpu->idle = idle;
cpu->handoff = NULL;

init->oncpu = cpu->id;
set_current_task(init);

load_final_page_table();
user_access_disable();

init->time_slice = TIME_SLICE_MAX;
init->ktime = jiffies_get();

__atomic_store_n(&cpu->online, true, __ATOMIC_RELEASE);
```

之后按 CPU 角色分支：

- **boot hart**：`local_irq_enable(); task_init_main();`
- **secondary hart**：把静态 init task 标记为 `TASK_STATE_ZOMBIE`，调用 `task_yield()` 切换到动态 idle task；静态 boot task 从此 park。

`task_create_idle()` 创建的 idle task：

- 有自己分配的 16 KiB 内核栈；
- 陷阱帧在栈顶；
- 初始上下文入口是 `task_idle_entry()`；
- `task_idle_entry()` 首次运行先调用 `complete_task_switch()`，释放调度器持有的锁和 handoff 状态，再进入：

```c
for (;;) {
    local_irq_enable();
    cpu_idle();          /* WFI */
    local_irq_disable();
    task_yield();
}
```

### 4.3 CPU0：`task_init_main()`

`task_init_main()` 运行在 CPU0 的静态 init task 上：

1. 设置全局 `init_reaper = current_task()`；
2. **先创建用户 init task**，但保持 `TASK_STATE_NEW`，这样 PID 1 留给 `/sbin/init`；
3. 运行 `fs_init()`：
   - 注册 tmpfs、devtmpfs、brkfs、procfs、pipefs；
   - 扫描分区；
   - 建立 tmpfs 根并挂载 brkfs；
   - 建立 `/dev`、`/proc`，打开 `/dev/tty0` 并设置 fd 0/1/2；
4. 把 init task 的 `fsinfo`/`fdtable` 复制给用户 init；
5. 把用户 init 的 parent 设为 init task，加入 `children` 链表；
6. 把用户 init 状态设为 `RUNNING` 并 `task_join()`；
7. 进入循环：

```c
for (;;) {
    if (task_wait(-1, NULL, 0, NULL) < 0)
        ktime_nanosleep(100ms, NULL);
}
```

因此 CPU0 的静态 init task 同时承担：

- 启动期文件系统初始化；
- PID 1 的父任务；
- 孤儿进程回收者（`init_reaper`）；
- 空闲时的可阻塞内核任务。

它**不是** idle task。真正的 idle task 是每 CPU 动态分配的，用来在 init task 睡眠时作为调度 fallback。

### 4.4 用户 init 的 exec

用户 init task 的初始上下文是汇编标签 `ret_from_user_init`：

```asm
ret_from_user_init:
    call user_init_entry
    j ret_from_trap
```

`user_init_entry()`：

1. `complete_task_switch()`：释放调度器锁、处理 handoff、开启中断；
2. `do_execve("/sbin/init", argv, envp)`；
3. exec 失败：打印错误并 `task_exit_normal(1)`；
4. exec 成功：
   - 启动 console log 线程；
   - 启动 klogd；
   - 启动 writeback 线程；
   - `bring_up_secondaries()`；
   - `prepare_to_ret_to_user(current_task()->frame)`；
5. 返回汇编，跳到 `ret_from_trap`，`sret` 进入用户态。

用户 init 是普通任务：

- 有 PID 1；
- parent 是静态 init task（PID 0）；
- 可以退出；
- 退出后由 `init_reaper` 回收；
- 内核不再有 `initial_task` 全局指针或特殊 exit/signal 处理。

## 5. 二级 CPU 启动

启动顺序：

1. CPU0 的 PID 1 在 exec 成功后调用 `bring_up_secondaries()`；
2. `smp_wake_secondary_harts()` 通过 SBI HSM `hart_start` 启动除 boot hart 之外的 hart；
3. 入口地址是 `symbol_phys(_secondary_start)`；
4. 二级 hart 执行 `_secondary_start`：
   - 设置自己的启动栈；
   - 加载早期页表；
   - 跳到高地址 `secondary_entry`；
5. `secondary_entry()`：

```c
cpu_init(hart_id);
setup_trap_entry();

load_final_page_table();

timer_init_hart();
trap_init();
irq_init_hart(hart_id);

local_irq_enable();

task_scheduler();
```

6. 二级 CPU 的 `task_scheduler()`：
   - 初始化自己的静态 init task；
   - 创建自己的动态 idle task；
   - 把静态 init task park（标记成 ZOMBIE）；
   - 通过 `task_yield()` 正常调度到 idle task；
   - 之后只作为 idle/运行队列 CPU 工作。

二级 CPU 不运行 `start_kernel()`，也不运行 `do_initcalls()`；全局初始化已经由 boot hart 完成。

## 6. 陷阱入口与返回

### 6.1 陷阱帧

`struct trap_frame` 固定 304 字节：

| 偏移 | 内容 |
| --- | --- |
| 0..255 | 32 个通用寄存器 |
| 256 | `sstatus` |
| 264 | `sepc` |
| 272 | `scause` |
| 280 | `stval` |
| 288 | `kernel_tp`（软件保存的内核 per-CPU 指针） |
| 296 | padding |

内核栈布局：

```text
kstack_top
  +----------------------+
  | trap_frame 304 B     |  task->frame
  +----------------------+
  | context 112 B        |  task->kernel_sp（新建任务）
  +----------------------+
  | ... 可用内核栈 ...    |
  +----------------------+
kstack_base
```

新建任务时 `task_create()` 先减去 `TRAP_FRAME_SIZE`，再减去 `CONTEXT_SIZE`，并保证 16 字节对齐。

### 6.2 `sscratch` 协议

不变量：

- 内核态：`sscratch == 0`；
- 用户态：`sscratch == 当前任务内核栈顶`。

`trap_entry` 首先执行：

```asm
csrrw sp, sscratch, sp
bnez sp, .Ltrap_from_user
```

- 从内核陷阱：`sscratch` 为 0，交换后 `sp = 0`，再交换回来；在旧内核 `sp` 下方分配陷阱帧。
- 从用户陷阱：`sscratch` 是内核栈顶，交换后 `sp = 内核栈顶`，`sscratch = 用户 sp`；减去 `TRAP_FRAME_SIZE` 后保存用户 `sp`，并从 `frame->kernel_tp` 恢复内核 `tp`。

保存寄存器后立即：

```asm
csrw sscratch, zero
```

这保证嵌套内核陷阱不会被误判为用户陷阱。

### 6.3 `trap_handler()`

- `scause` 最高位区分中断/异常；
- `sstatus.SPP` 区分来自内核还是用户；
- 来自用户的陷阱会设置 `sstatus.SUM`，允许内核访问用户地址；
- 系统调用从 `frame->a7` 取号，参数在 `a0..a5`，返回值写回 `a0`；
- 页错误可走 `uvm_handle_fault()` 的按需分页/COW；
- 用户异常默认发送信号，内核异常 panic；
- 用户陷阱返回前会 `prepare_to_ret_to_user()`、投递待处理信号、更新时间统计。

### 6.4 `ret_from_trap`

关键步骤：

1. 把当前内核 `tp` 写回 `frame->kernel_tp`；
2. 恢复 `sstatus`、`sepc`；
3. 根据返回目标设置 `sscratch`：
   - 回用户：`sscratch = frame + TRAP_FRAME_SIZE`（内核栈顶）；
   - 回内核：`sscratch = 0`；
4. 恢复通用寄存器，最后恢复 `sp`，执行 `sret`。

### 6.5 已知限制：用户 `tp`

当前 `trap_entry` 的用户路径在恢复内核 `tp` 后才保存 `x4`，因此陷阱帧中保存的 `tp` 是内核 per-CPU 指针，而不是用户原来的线程指针。没有 TLS 的用户程序不受影响；依赖 `tp` 的 TLS 用户态需要后续修复。

## 7. 上下文切换

### 7.1 保存的上下文

`struct context`：

```c
struct context {
    unsigned long s0..s11;
    unsigned long ra;
    unsigned long __pad;
};
```

`__switch_to(unsigned long *old_sp, unsigned long *new_sp)`（`src/arch/riscv/kernel/switch_context.S`）：

1. `t0 = sp - 112`；
2. `*old_sp = t0`；
3. 保存 `s0..s11` 和 `ra`；
4. 从 `*new_sp` 读取 `t0`；
5. `sp = t0 + 112`；
6. 恢复 `s0..s11` 和 `ra`；
7. `ret`。

### 7.2 `context_switch(prev, next)`

通用调度器只使用：

```c
void context_switch(struct task *prev, struct task *next);
```

RISC-V 实现在 `src/arch/riscv/kernel/cpu.c`：

```c
void context_switch(struct task *prev, struct task *next)
{
    assert(prev != NULL);
    assert(prev != next);

    __switch_to(&prev->kernel_sp, &next->kernel_sp);
}
```

`__switch_to()` 声明在 `src/arch/riscv/include/arch/context.h`，不再暴露在通用 `brk/cpu.h` 中。

### 7.3 `task_sched()` 与 handoff 协议

调度器核心不变量：

```text
切换前：curr->lock 由调度器持有
切换时：curr 仍是 "oncpu = cpu"
切换后：next 的入口/恢复路径调用 task_sched_resume()
        → prev->oncpu = -1
        → raw_spin_unlock(&prev->lock)
```

流程：

1. 处理 `curr` 状态：
   - RUNNING 且不是 idle：重新加入运行队列；
   - SLEEPING：已在睡眠队列中；
   - ZOMBIE：不再入队；
2. 从运行队列取 `next`，为空则使用 `cpu->idle`；
3. 若 `next == curr`，复位时间片并返回，不做上下文切换；
4. 记录系统时间；
5. `cpu->handoff = curr`；
6. 锁定 `next->lock`，设置 `next->oncpu`、`current`、`kernel_tp`；
7. 切换页表/SUM；
8. `context_switch(curr, next)`；
9. 回到 `task_sched()` 后调用 `task_sched_resume()`。

新建任务的入口（用户 init、idle、kthread、fork 返回）必须调用 `complete_task_switch()`，它等价于：

```c
task_sched_resume();
raw_spin_unlock(&current_task()->lock);
local_irq_enable();
```

## 8. 启动阶段的关键不变量

- 在内核态中 `sscratch` 必须为 0；在用户态中必须等于当前任务内核栈顶。
- 每个 hart 的启动栈顶部 304 字节预留给 init task 陷阱帧。
- `task_scheduler()` 在把 CPU 标记为 online 之前必须已经设置 `current`。
- 第一次真实上下文切换必须有一个有效的 `prev`；静态 init task 提供了这个 `prev`，不需要专门的“第一次切换”分支。
- idle task 必须与 init task 分离：init task 会睡眠，idle 不能睡眠。
- `init_task` 睡眠时，调度器必须能切换到真正的 idle task，避免 `task_sched()` 的 `next == curr` 早退路径把睡眠状态留在当前任务上。
- 二级 CPU 不执行全局 initcalls，只做自己的架构/IRQ/调度初始化。
- initcall 阶段没有调度器，不能睡眠，不能依赖 `current_task()`。
