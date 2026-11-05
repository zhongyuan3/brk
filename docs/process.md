# 进程、调度器、信号与同步

本文描述 BRK 的任务模型、调度器、上下文切换协议、fork/exec/exit/wait、信号、系统调用接口和同步原语。启动阶段的任务创建细节见 [boot.md](boot.md)。

相关源码：

- `src/include/brk/task_types.h`
- `src/include/brk/task.h`
- `src/include/brk/cpu.h`
- `src/kernel/proc/task.c`
- `src/kernel/proc/sched.c`
- `src/kernel/proc/sysproc.c`
- `src/kernel/signal.c`
- `src/kernel/sync/spinlock.c`
- `src/kernel/sync/sleeplock.c`
- `src/kernel/syscall.c`
- `src/arch/riscv/kernel/trap.c`
- `src/arch/riscv/kernel/trap_entry.S`

## 1. 任务模型

### 1.1 `struct task`

`struct task` 把调度状态、身份、资源和父子关系放在同一个对象中。字段按保护方式分组：

| 字段组 | 保护 | 说明 |
| --- | --- | --- |
| `list` | `tasks_lock` | 全局任务链表 |
| `queue` | 运行队列锁或 `sleep_queue_lock` | 同一时刻只属于一个队列 |
| `parent`、`children`、`child` | `wait_lock` | 父子关系、reparent、wait |
| `lock` | 自身 | 保护 `state`、`chan`、`kernel_sp`、`exit_status` |
| `state` | `lock` | NEW/RUNNING/SLEEPING/ZOMBIE |
| `kernel_sp` | `lock` | 保存的上下文基址 |
| `chan` | `lock` | 睡眠通道 |
| `mm` | 引用计数 | 用户地址空间 |
| `fdtable` | 引用计数 | 文件描述符表 |
| `fsinfo` | 引用计数 | root/cwd |
| `sigactions` | 表内锁 | 信号动作和 pending/blocked |
| `rsrc_usage` | 自身锁 | 累计 user/system/children 时间 |
| `frame` | 私有 | 用户陷阱帧固定地址 |
| `kstack_base/top` | 私有 | 内核栈范围 |
| `oncpu` | 私有 | 当前 CPU，-1 表示不在 CPU 上 |
| `cpu` | 私有 | 最近选择/分配的 CPU |
| `is_init_task` | 私有 | 静态启动任务，禁止销毁 |

`struct cpu` 保存每 CPU 调度状态：

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

### 1.2 任务状态

```text
TASK_STATE_NEW
      │ task_join 前由调用者设为 RUNNING
      ▼
TASK_STATE_RUNNING ── task_sleep ──► TASK_STATE_SLEEPING
      │                                   │ task_wake_*
      │                                   ▼
      │                              TASK_STATE_RUNNING
      │
      │ task_exit
      ▼
TASK_STATE_ZOMBIE
      │ parent task_wait/task_destroy
      ▼
   释放
```

- `NEW`：已创建但尚未入运行队列，不能调度。
- `RUNNING`：可运行或正在运行。
- `SLEEPING`：已挂在睡眠队列，等待 `task_wake_all()` / `task_wake_spec()`。
- `ZOMBIE`：已退出，等待父任务回收。

## 2. 任务生命周期

### 2.1 `task_create()`

`task_create()` 的执行顺序：

1. 分配 `struct task`（slab zeroed）；
2. 初始化链表、队列、父子链表、任务锁；
3. `state = NEW`，`oncpu = -1`，`cpu = this_cpuid()`；
4. 分配 PID；
5. 设置 `tgid`、`pgid`、`sid`；
6. 设置默认 `umask=022`、完整 capability；
7. 分配 16 KiB 内核栈；
8. 在栈顶构造陷阱帧，并在其下构造初始 `struct context`；
9. `ctx->ra = args->fn` 或 `default_new_task_entry`；
10. 分配/获取 `mm`、`fdtable`、`fsinfo`、`rsrc_usage`；
11. 初始化信号表；
12. 加入全局 `tasks` 链表；
13. 返回仍处于 `NEW` 的任务。

调用者必须：

```c
task->state = TASK_STATE_RUNNING;
task_join(task);
```

或者使用已有辅助函数（`kthread_join`、`task_join`）。

### 2.2 静态 init task 与 PID 1

- 每个 CPU 有一个静态 `init_tasks[cpuid]`，使用该 hart 的启动栈；
- PID/TGID/PGID/SID 都是 0；
- `is_init_task = true`，`task_destroy()` 会拒绝销毁它；
- CPU0 的 init task 运行 `task_init_main()`，先创建用户 init（保持 PID 1），再执行 `fs_init()`；
- 文件系统初始化完成后，用户 init 被设为 `RUNNING` 并加入运行队列；
- CPU0 的 init task 之后循环 `task_wait(-1)`，成为 `init_reaper`；
- 二级 CPU 的静态 init task 被标记为 zombie 并 park，之后不再运行。

用户 init：

- PID 1；
- parent 是静态 init task；
- 是普通任务，没有全局 `initial_task` 指针；
- 可以退出，退出后由 `init_reaper` 回收。

### 2.3 `task_fork()`

`task_fork()` 复制当前任务：

1. `task_create()` 创建子任务，入口为 `ret_from_fork`；
2. `uvm_space_copy()` 复制地址空间：
   - 匿名可写页变成 COW；
   - 用户栈立即复制；
   - 堆和 mmap 区域按 COW 规则复制；
   - 共享文件映射增加页缓存引用；
3. 复制陷阱帧；
4. `signal_copy()` 复制信号动作和 blocked 掩码，清空 pending；
5. 复制凭据、组、umask、capability；
6. `fdtable_copy()` 复制文件描述符表（文件对象引用增加）；
7. `fsinfo_copy()` 复制 root/cwd；
8. 子任务 `fork()` 返回值为 0；
9. 在 `wait_lock` 下把子任务加入父任务 `children`；
10. 设置 `RUNNING`、`newly_forked = true`，调用 `task_join()`。

子任务首次运行走：

```text
ret_from_fork
  └─ task_schedlue_tail()
       └─ complete_task_switch()
            └─ ret_from_trap
```

### 2.4 `execve`

`do_execve()`：

- 读取并验证 ELF：
  - 只支持 `ELFCLASS64`、`ET_EXEC`；
  - 不支持 PIE、解释器、脚本；
- 为每个 `PT_LOAD` 立即分配并映射页；
- 建立新的 `uvm_space`；
- 设置堆起点为最高段末尾，初始大小 0；
- 分配 1 MiB 用户栈，随机化 1–32 页偏移；
- 把 argv/envp 写入新栈；
- 切换页表，替换当前任务的 `mm`；
- `signal_reset()`；
- 根据 setuid/setgid 位更新凭据和 capability；
- 设置陷阱帧 `sepc` 和 `sp`。

exec 不改变：

- PID/TID；
- 父任务、pgid/sid；
- fdtable；
- fsinfo（root/cwd）；
- 子任务列表；
- 时间统计对象。

### 2.5 退出与僵尸

`task_exit(status)`：

1. 丢弃 fdtable 和 fsinfo 引用（此时关闭文件/释放路径）；
2. 在 `wait_lock` 下：
   - 把子任务 reparent 到 `init_reaper`；
   - 把整个 children 链表 splice 到 reaper；
3. 如果发生 reparent，唤醒 reaper；
4. 锁定自身，设置 `state = ZOMBIE`、`exit_status`；
5. 调用 `task_sched()`，不再返回。

退出状态编码：

- `task_exit_normal(code)`：`status = (code & 0xff) << 8`
- `task_exit_signal(sig)`：`status = sig & 0x7f`

僵尸保留：

- task 结构、全局链表项、PID、内核栈、mm；
- 直到父任务 `task_wait()` 回收后，`task_destroy()` 才释放。

### 2.6 wait 与销毁

`task_wait(pid, status, options, rus)`：

- 只处理当前任务的直接子任务；
- 忽略 `options`、`rusage`；
- 找到 `state == ZOMBIE && oncpu == -1` 的子任务后：
  - 写回状态；
  - 把子任务时间累计到父任务；
  - 从 children 链表移除；
  - `task_destroy(child)`；
- 没有可回收子任务时调用 `task_sleep(parent, &wait_lock)`。

`task_destroy()`：

- 拒绝 `is_init_task`；
- 从全局任务链表移除；
- 释放信号表、资源统计、fsinfo/fdtable、mm；
- 释放内核栈；
- 释放 PID（当前是 no-op）和 task slab。

### 2.7 PID 分配

```c
static pid_t curr_pid = 1;
pid_t pid_alloc(void) { return curr_pid++; }
void pid_free(pid_t pid) { (void)pid; }
```

- PID 0 保留给静态 init/idle；
- PID 1 是用户 init；
- PID 单调递增，不复用；
- 没有 PID 哈希表，查找是全局任务链表线性扫描。

## 3. 调度器

### 3.1 运行队列与 CPU 选择

每 CPU：

- `run_queue`：FIFO 链表；
- `nr_running`：队列中的任务数；
- `current`：当前任务；
- `idle`：idle 任务。

`task_select_cpu()`：

- 只考虑 `online == true` 的 CPU；
- `cpu_load()` = `nr_running` + （current 存在且不是 idle ? 1 : 0）；
- 选择负载最低的 CPU；
- 使用 `next_cpu_hint` 轮转，避免总是选 CPU0。

`task_join(task)`：

- 如果 `oncpu < 0`：选择 CPU；
- 如果 `oncpu >= 0`：必须留在当前 CPU，直到上下文切换完成；
- 把任务加入目标 CPU 的运行队列；
- 如果目标不是当前 CPU，发送 IPI。

### 3.2 `task_sched()`

前置条件：

```c
assert(!local_irq_enabled());
assert(curr->lock.locked);
assert(preempt_count() == 0);
assert(curr->oncpu == cpu->id);
```

流程：

1. `set_current_task(NULL)`；
2. 根据 `curr->state`：
   - RUNNING 且 `curr != cpu->idle`：`task_join(curr)`；
   - SLEEPING：已在睡眠队列，不再入队；
   - ZOMBIE：不再入队；
3. 从运行队列取 next；为空则 `cpu->idle`；
4. 若 `next == curr`：复位时间片，恢复 current，直接返回；
5. 记录 `curr` 的系统时间；
6. `cpu->handoff = curr`；
7. 锁定 `next->lock`；
8. 设置 `next->oncpu`、`current`、`kernel_tp`；
9. 切换页表：
   - 有 mm：`switch_pgtable(mm->pgd)` + `user_access_enable()`；
   - 无 mm：`load_final_page_table()` + `user_access_disable()`；
10. 复位 `next` 时间片和 `ktime`；
11. `context_switch(curr, next)`；
12. 返回后 `task_sched_resume()`。

### 3.3 handoff 协议

```text
task_sched(curr -> next):
    curr->lock 仍被持有
    cpu->handoff = curr
    next->lock 被调度器获取
    context_switch(curr, next)
        │
        ├─ 如果 next 是新任务：
        │      next 入口先 complete_task_switch()
        │      → task_sched_resume()
        │      → prev->oncpu = -1
        │      → raw_spin_unlock(prev->lock)
        │
        └─ 如果 next 之前运行过：
               next 从自己的 task_sched() 返回
               → task_sched_resume()
               → 同样释放 prev
```

关键点：

- 出任务的 `lock` 永远跨上下文切换保持持有；
- `oncpu != -1` 表示该任务的内核栈和上下文仍可能被 CPU 使用；
- 回收僵尸必须等 `oncpu == -1`；
- `task_sched_resume()` 在僵尸真正离开 CPU 后才唤醒父任务。

### 3.4 `task_yield()` 与 `task_sleep()`

`task_yield()`：

```c
flags = local_irq_save();
raw_spin_lock(&task->lock);
task_sched();
raw_spin_unlock(&task->lock);
local_irq_restore(flags);
```

`task_sleep(chan, cond_lock)`：

前置条件：

- 中断关闭；
- 调用者持有条件锁；
- `preempt_count == 0`。

流程：

1. 锁定 `task->lock`；
2. 设置 `state = SLEEPING`、`chan = chan`；
3. 在仍持有条件锁时把任务加入全局 `sleep_queue`；
4. 释放条件锁；
5. `task_sched()`；
6. 被唤醒后清空 `chan`；
7. 释放 `task->lock`；
8. 重新获取条件锁；
9. 调用者重新检查条件。

这解释了为什么所有睡眠调用都必须放在循环里。

### 3.5 唤醒

`task_wake_all(chan)`：

- 遍历 `sleep_queue`；
- 对 `state == SLEEPING && chan == chan` 的任务；
- 使用 `spin_trylock_irqsave(&p->lock)` 避免与 `task_sleep()` 的锁顺序死锁；
- 设置 RUNNING、清空 chan、从睡眠队列移除；
- 释放睡眠队列锁后调用 `task_join()`。

`task_wake_spec(task)` 类似，但只唤醒指定任务。

### 3.6 时间片与抢占

- 定时器 tick 间隔 1 ms；
- `TIME_SLICE_MAX = 5`，约 5 ms；
- 定时器中断递减当前任务时间片；
- 只有 `preempt_count == 0` 时才 `task_yield()`；
- IPI（软件中断）也直接触发 `task_yield()`；
- 没有优先级、没有 CFS、没有调度类、没有周期性负载均衡。

## 4. Idle 任务

`task_create_idle(cpuid)`：

- 直接分配 `struct task`，不经过 `task_create()`；
- 无 PID、无 mm、无 fdtable、无 fsinfo、无信号表；
- 分配自己的 16 KiB 内核栈；
- 入口 `task_idle_entry()`：

```c
complete_task_switch();
for (;;) {
    local_irq_enable();
    cpu_idle();          /* WFI */
    local_irq_disable();
    task_yield();
}
```

idle task：

- 不在运行队列，也不在全局任务链表；
- 当运行队列为空时作为 `cpu->idle` fallback；
- 如果当前已经是 idle，`next == curr` 快路径避免无意义切换。

## 5. 内核线程

`kthread_create(fn, data)`：

- 创建 `struct task`，入口 `kthread_return`；
- 保存 `kthrd_fn/kthrd_data`；
- 返回 `NEW` 任务。

`kthread_join(kthread)`：

- 不是等待结束，而是设置 RUNNING 并加入运行队列；
- 调用后立即返回。

`kthread_return()`：

1. `complete_task_switch()`；
2. 调用 `kthrd_fn(data)`；
3. `task_exit_normal(0)`。

内核线程通常没有父任务，也没有子任务。如果它们退出，会成为无人回收的 zombie；当前实现中多数 kthread 是永久循环，不会退出。

## 6. 信号

### 6.1 表示

`struct sigaction_table`：

```c
struct sigaction actions[NSIG];
spinlock_t lock;
refcnt_t refcnt;
```

每任务信号状态：

- `pending`：64 位位图；
- `blocked`：64 位位图；
- `in_handler`：是否正在执行用户处理函数；
- `sigframe_sp`：当前信号帧用户栈地址。

信号不排队：同一信号多次发送会合并成一位。

### 6.2 发送

`signal_send(task, sig)`：

- 锁定 task；
- 忽略 zombie；
- 锁定 sigactions；
- 设置 pending 位；
- `task_wake_spec(task)`。

`signal_do_kill(pid, sig)` 支持：

- `pid > 0`：按 `pid` 查找单任务；
- `pid == -1`：所有线程组 leader；
- `pid == 0`：当前任务所在进程组；
- `pid < -1`：进程组 `-pid`；
- 权限：`CAP_KILL` 或 `target->euid == current->euid`；
- `sig == 0`：只做存在性/权限检查。

### 6.3 投递

`get_next_signal_locked()`：

- `deliver = pending & ~blocked`；
- 如果正在用户 handler 中，只允许 `SIGKILL` 和 `SA_NODEFER` 的信号；
- 返回编号最小的可投递信号。

`signal_deliver_one()`：

- `SIGKILL` 或默认终止动作：`task_exit_signal(sig)`；
- `SIG_IGN`：清除 pending；
- 默认忽略：清除 pending；
- 用户 handler：
  - 在用户栈上构造 `user_sigframe`；
  - 保存完整陷阱帧；
  - 设置 SP/PC/参数；
  - 根据 `SA_NODEFER`/`sa_mask` 更新 blocked；
  - 设置 `in_handler`。

投递时机：

- 用户陷阱返回前；
- 用户 `ecall` 前；
- 阻塞代码显式检查 `signal_pending()` 时。

### 6.4 `sigreturn`

`signal_do_sigreturn()`：

- 验证 `sigframe_sp`；
- 从用户栈恢复陷阱帧和 blocked；
- 清除 `in_handler`、`sigframe_sp`；
- 返回值写回 `a0`。

### 6.5 默认动作

默认忽略：

- `SIGCHLD`、`SIGURG`、`SIGWINCH`、`SIGCONT`

其余已定义信号默认终止。当前没有：

- 停止/继续作业控制；
- core dump 标记；
- `SIGCHLD` 自动生成；
- `SA_RESTART`；
- `siginfo`/信号队列。

## 7. 系统调用

### 7.1 ABI

- 系统调用号：`a7`；
- 参数：`a0..a5`；
- 返回值：`a0`；
- `syscall()` 通过 `systable[num]` 分发；
- 越界或空洞返回 `-ENOSYS`。

### 7.2 参数访问

`syscall_arg_raw/int/ptr` 直接读取 `current_task()->frame` 中的对应寄存器。

用户指针访问有两种方式：

- 通过 `copy_from_user`/`copy_to_user`；
- 直接解引用（当前代码中仍有不少路径这样做，依赖 `sstatus.SUM` 和页错误处理）。

### 7.3 进程相关系统调用

- `fork`、`execve`、`exit`、`wait4`
- `getpid/gettid/getppid`
- `kill`
- `setuid/setgid/setreuid/...`
- `setpgid/getpgrp/setsid/getpgid/getsid`
- `rt_sigaction/rt_sigprocmask/rt_sigreturn`
- `sched_yield`
- `mmap/munmap/mprotect/mremap/msync/brk`

未实现或返回 `-ENOSYS` 的常见接口包括：

- `clone`、`clone3`
- `vfork`、`exit_group`
- `waitid`
- `tgkill/tkill`
- `sigaltstack`
- `getrusage`
- `fcntl/flock`
- `futex`

## 8. 同步原语

### 8.1 spinlock

```c
typedef struct spinlock {
    volatile unsigned int locked;
    cpuid_t cpuid;
    const char *name;
} spinlock_t;
```

变体：

| 接口 | 行为 |
| --- | --- |
| `spin_lock/unlock` | 互斥 + preempt_disable/enable |
| `spin_trylock` | 成功时 preempt_disable |
| `raw_spin_lock/unlock` | 不改变 preempt/IRQ，仅调度器 handoff |
| `spin_lock_irqsave` | 关中断 + raw spin |
| `spin_unlock_irqrestore` | raw unlock + 恢复中断 |
| `spin_trylock_irqsave` | 保存中断状态后尝试 |

使用规则：

- 中断上下文访问的数据必须用 irqsave 变体；
- 临界区可能睡眠时使用 irqsave 变体，并在 `task_sleep` 前后由调用者管理锁；
- 持有普通 spinlock 时不能睡眠；
- 调度器内部跨上下文切换的锁使用 raw 变体。

### 8.2 sleeplock

```c
typedef struct sleeplock {
    volatile unsigned int locked;
    spinlock_t lock;
    pid_t pid;
    const char *name;
} sleeplock_t;
```

`sleeplock_acquire()`：

- 禁止同一任务重复获取；
- 在内部 spinlock 上循环；
- 被占用时调用 `task_sleep(lock, &lock->lock)`；
- 获取后记录 holder PID。

`sleeplock_release()`：

- 校验 holder；
- 释放并 `task_wake_all(lock)`。

典型用途：

- 文件系统目录/文件操作；
- 挂载拓扑修改；
- 页缓存 I/O 串行化。

## 9. 锁顺序与不变量

当前代码中常见锁顺序：

1. `task->lock` → `sleep_queue_lock`（`task_sleep`）；
2. `sleep_queue_lock` → try `task->lock`（唤醒路径）；
3. `tasks_lock` → `target->lock` → `sigactions->lock`（信号/任务查询）；
4. `task->lock` → `sigactions->lock`（`signal_send`）；
5. `wait_lock` → `child->lock` → `rsrc_usage->lock`（`task_wait`）；
6. 运行队列锁与任务锁：`task_join()` 先运行队列锁，释放后再处理任务锁；上下文切换时持有出/入任务锁；
7. `tasks_lock` 用于查找/固定任务，`task_destroy()` 在同一锁下从全局链表移除。

其他不变量：

- 调度路径必须关闭中断；
- `task_sleep()` 必须由持有条件锁的调用者调用；
- `preempt_count` 必须为 0 才能进入调度器；
- 出任务的锁跨上下文切换持有，由入任务释放；
- `task->oncpu == -1` 才允许回收僵尸；
- 静态 init task 永不销毁。

## 10. 当前限制

- FIFO 调度，无优先级/公平性/负载均衡；
- `task->cpu` 不是硬亲和性；
- `task_wait()` 不支持 `WNOHANG`、`rusage`、进程组等待，也不检查 pending 信号；
- `SIGCHLD` 不自动发送；
- `clone` 未实现，thread-group 字段基本未使用；
- 内核线程退出后没有父任务回收；
- `default_new_task_entry()` 不调用 `complete_task_switch()`，如果未来有无 `fn` 的任务直接使用该入口会破坏 handoff 协议；
- 信号投递和睡眠存在经典的 check/sleep 竞争窗口，部分阻塞路径依赖定时器 tick 兜底。
