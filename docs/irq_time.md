# 中断、时钟与日志

本文描述 BRK 的 IRQ 框架、PLIC 驱动、定时器与时间保持，以及 printk/console/klogd/panic 路径。日志环形缓冲区的协议细节另见 [kernel_log.md](kernel_log.md)。

相关源码：

- `src/include/brk/irq.h`、`irq/irqdesc.h`、`irq/irqdomain.h`、`irq/irqchip.h`
- `src/kernel/irq/irqdesc.c`、`handle.c`、`irqdomain.c`、`irqchip.c`、`selftest.c`
- `src/driver/irqchip/sifive_plic.c`
- `src/arch/riscv/kernel/trap.c`
- `src/arch/riscv/kernel/timer.c`
- `src/arch/riscv/kernel/rtc.c`
- `src/kernel/time/clocksource.c`、`timekeeper.c`、`ktime.c`、`systime.c`
- `src/kernel/log/printk.c`、`console.c`、`klog.c`
- `src/kernel/panic.c`
- `src/driver/rtc/google_goldfish_rtc.c`

## 1. IRQ 框架

### 1.1 描述符

`NR_IRQS = 256`，静态数组 `irq_desc_table[NR_IRQS]`。

```c
struct irq_data {
    unsigned int irq;       /* virtual IRQ */
    unsigned int hwirq;     /* controller 硬件号 */
    struct irq_chip *chip;
    struct irq_domain *domain;
    void *chip_data;
    unsigned int hart;
    unsigned int type;
};

struct irqaction {
    irqreturn_t (*handler)(int irq, void *dev_id);
    void *dev_id;
    unsigned long flags;
    const char *name;
    struct irqaction *next;
};

struct irq_desc {
    struct irq_data irq_data;
    struct irqaction *action;
    spinlock_t lock;
    irq_flow_handler_t handle_irq;
    unsigned int depth;
    unsigned int count;
};
```

IRQ 0 保留不可分配。`irq_alloc_desc()` 从 1 开始扫描：

- 标记 allocated；
- 清零 descriptor；
- 初始化锁；
- 默认 flow handler `handle_simple_irq`；
- `depth = 1`，表示初始屏蔽。

### 1.2 申请与开关

`request_irq(irq, handler, flags, name, dev_id)`：

- 要求 descriptor 已分配；
- 要求已经设置 `irq_data.chip`；
- 分配 `irqaction`；
- 在 `desc->lock` 下：
  - 已存在 action 且新请求不是 `IRQF_SHARED` → `-EBUSY`；
  - 已有 action 必须都是 shared；
  - 重复 `dev_id` → `-EINVAL`；
  - 头插 action；
- 除非 `IRQF_NO_AUTOEN`，自动 `enable_irq()`。

`enable_irq()` / `disable_irq()`：

- 使用嵌套计数 `depth`；
- enable 从 1→0 时调用 `chip->irq_unmask`；
- disable 从 0→1 时调用 `chip->irq_mask`。

`free_irq()`：

- 移除匹配 `dev_id` 的 action；
- 最后一个 action 移除后 disable；
- 不等待正在执行的 handler。

### 1.3 Flow handler

`handle.c`：

- `handle_simple_irq`：加锁、计数、调用所有 action；
- `handle_level_irq`：mask → ack → action → unmask；
- `handle_edge_irq`：ack → action → eoi。

所有 handler 都在 `desc->lock` + 中断关闭下运行，不能睡眠，也不能对同一 descriptor 递归 `request_irq/free_irq`。

PLIC 使用 `handle_simple_irq`。

### 1.4 IRQ domain

```c
struct irq_domain {
    const char *name;
    int fdt_node;
    unsigned int size;
    unsigned int *revmap;   /* hwirq -> virq */
    struct irq_domain_ops *ops;
    void *host_data;
    struct list_head list;
};
```

- `irq_domain_add_linear()` 建立线性 revmap；
- `irq_create_mapping(domain, hwirq, type)`：
  - 已映射则返回旧 virq；
  - 分配 virq；
  - 填充 `irq_data.hwirq/domain/type/hart`；
  - 调用 `domain->ops->map`；
  - 保存 `revmap[hwirq]`。
- `irq_of_parse_and_map()`：
  - 沿设备树父节点找 `interrupt-parent`；
  - 读取 `#interrupt-cells` 和 `interrupts[index]`；
  - 调用 `xlate`；
  - 创建映射。

当前 revmap 没有并发锁，假设映射发生在启动期。

### 1.5 irqchip 注册与分发

`irqchip.c`：

- `irqchip_register_driver()`：注册 compatible 匹配表；
- `irq_register_controller()`：注册 controller；
- `irq_init_hart_all(hart)`：对每个 controller 调用 `init_hart`；
- `irq_init_hart()`：公开包装；
- `irq_dispatch_external(hart)`：遍历所有 controller 的 `handle_irq`；
- `irq_handle_external()`：trap 层入口。

`irqchip_init()`（postcore initcall）：

1. 扫描 FDT 中有 `interrupt-controller` 属性的节点；
2. 按 compatible 匹配已注册 driver；
3. 调用 `probe`；
4. `irq_init_hart_all(this_cpuid())`。

### 1.6 PLIC

`siFive PLIC` 兼容字符串：

- `"sifive,plic-1.0.0"`
- `"riscv,plic0"`

MMIO：

| 区域 | 偏移 |
| --- | --- |
| priority | `0x000000` |
| enable | `0x002000` |
| threshold | `0x200000` |
| claim/complete | `0x200004` |

S-mode context：

```c
plic_ctx(hart) = 2 * hart + 1
```

- enable base: `0x2000 + ctx * 0x80`
- threshold/claim: `0x200000 + ctx * 0x1000`

Probe：

- 读取 `reg`；
- `ioremap`；
- 读取 `riscv,ndev`（默认 1024，最小 2）；
- 创建 linear domain；
- 注册 controller；
- 运行 PLIC 自测；
- 打印源数量。

映射：

- `xlate`：hwirq = `intspec[0]`，type = `intspec[1]` 或 level high；
- `map`：
  - 拒绝 hwirq 0 或 >= ndev；
  - 设置 `plic_chip` + `handle_simple_irq`；
  - 记录 chip_data；
  - 设置 priority 1（priority 0 表示永不触发）。

中断处理：

1. 读取当前 hart 的 claim 寄存器；
2. 0 → spurious；
3. 非 0 → `irq_find_mapping()` → `generic_handle_irq()`；
4. 写回 claim/complete 寄存器；
5. 返回是否处理了中断。

`plic_irq_mask/unmask` 使用 `irq_data.hart` 选择上下文。`irq_set_affinity` 会 mask 旧 hart、修改 hart、unmask 新 hart。

### 1.7 外部中断路径

```text
trap_handler
  └─ handle_interrupt(INTERRUPT_SUPERVISOR_EXTERNAL)
       └─ irq_handle_external(this_cpuid())
            └─ irq_dispatch_external
                 └─ plic_handle_irq
                      ├─ claim
                      ├─ irq_find_mapping
                      ├─ generic_handle_irq
                      │    └─ handle_simple_irq
                      │         └─ action->handler
                      └─ complete
```

### 1.8 IPI

`send_ipi(cpuid)`：

- 检查 CPU id 范围和 `online`；
- 通过 SBI IPI 扩展设置目标 hart 的 `SSIP`；
- 如果 IPI 不可用，目标 CPU 的定时器 tick 最终会检查运行队列。

目标 CPU 收到 supervisor software interrupt：

- 清除 `sip.SSIP`；
- 如果 `preempt_count() == 0`，调用 `task_yield()`。

## 2. 时间保持

### 2.1 Clocksource

`clocksource.c`：

- 默认 timebase 10 MHz；
- `clocksource_init()` 从设备树 `/cpus/timebase-frequency` 读取；
- `timer_init()` 和 `timekeeper` 使用该频率。

### 2.2 定时器

`timer.c`：

```c
timer_interval = timebase_freq / 1000;   /* 1 ms */
program_next_tick() {
    sbi_set_timer(timer_read() + timer_interval);
}
```

`timer_read()` 读取 RISC-V `time` CSR。

`handle_tick()`：

- 只有 boot hart：
  - `timekeeper_tick()`：全局 jiffies++；
  - `tcp_timer_tick()`；
- 所有 hart：
  - `task_wake_all(&jiffies)`；
  - 重新编程下一次 tick。

### 2.3 timekeeper

状态：

- `mono_base_cycles`：启动时硬件计数器；
- `wall_to_mono_ns`：单调时间到墙钟的偏移；
- `jiffies`：全局 tick 计数；
- `tk_lock`。

函数：

- `timekeeper_mono_ns()`：`cycles_to_ns(timer_read() - mono_base_cycles)`；
- `timekeeper_real()`：单调时间 + 墙钟偏移；
- `timekeeper_init()`：记录基准，RTC 可用时读取墙钟；
- `timekeeper_tick()`：jiffies++；
- `jiffies_get()`；
- `timekeeper_wait_chan()` 返回 `&jiffies`；
- `timekeeper_nanosleep(dur, rem)`：
  - 计算单调时间截止点；
  - 在 `tk_lock` 下循环；
  - 检查 `task_is_killed(current_task())`；
  - 未到截止点则 `task_sleep(&jiffies, &tk_lock)`；
  - 定时器 tick 会唤醒所有 `&jiffies` 睡眠者；
  - 信号中断返回 `-EINTR` 和剩余时间。

注意：

- jiffies 只在 boot hart 递增；
- 所有 hart 都会唤醒 `&jiffies` 睡眠者；
- `task_is_killed()` 实际是“任意可投递信号 pending”，不是仅 SIGKILL。

### 2.4 ktime / 时间系统调用

`ktime_t` 是纳秒有符号整数。`ktime.h` 提供 timespec/timeval 转换和 inode 时间 helpers。

系统调用：

- `gettimeofday`
- `settimeofday`（需要 `CAP_SYS_TIME`）
- `times`（任务累计时间，使用 jiffies 单位）
- `nanosleep`

### 2.5 RTC

`rtc.c`：

- 查找 Goldfish RTC；
- `ioremap`；
- `rtc_read_ns` / `rtc_set_ns`；
- RTC 不可用时墙钟从 0 开始。

## 3. 日志与 console

### 3.1 printk API

级别：

```text
KERN_EMERG 0
KERN_ALERT 1
KERN_CRIT 2
KERN_ERR 3
KERN_WARN 4   ← 默认
KERN_NOTICE 5
KERN_INFO 6
KERN_DEBUG 7
```

API：

- `printk`
- `vprintk`
- `_printk(level, ...)`
- `pr_emerg/alert/crit/err/warn/notice/info/debug`
- `printk_flush_pending()`
- `printk_console_set_async(bool)`
- `printk_panic_text()`

### 3.2 环形缓冲区

详细协议见 [kernel_log.md](kernel_log.md)。

关键参数：

- `PRINTK_RING_BUFFER_SIZE = 64 KiB`
- `PRINTK_MAX_PAYLOAD = 1024` 字节
- 记录头 16 字节
- `head/tail` 是单调递增的字节序号
- 记录不跨环形边界
- 空间不足时丢弃最旧记录
- writer 先移动 tail 再覆盖
- `head` 最后发布，表示记录完整

reader：

- 每个 console/klogd 有独立 cursor；
- cursor 落后 tail 时通过 `pending_lost` 记录丢失字节；
- 支持从最旧或最新开始读取。

### 3.3 console

`console.c`：

- console 注册/注销；
- 每个 console 有独立 reader 和 cursor；
- 启动早期 `printk_console_sync = true`，`printk` 后同步 flush 到 console；
- `console_log_start()` 切换到异步模式并创建 `klog-console` 线程；
- console 驱动通过 `put_char` 输出，ns16550a 提供 UART console；
- console 线程负责把 ring 中的记录送到所有 console。

### 3.4 klogd

`klog.c`：

- `klog_start()` 创建 `klogd` 线程；
- klogd 把日志写入 `/var/log/kernel.log`；
- 启动时机：用户 init exec 成功后，与 console log、writeback 一起启动；
- 在 klogd 打开日志文件之前，UART console 保持同步输出，因此 exec 失败等信息仍然可见。

### 3.5 panic

`panic()`：

- 格式化并写入日志 ring；
- 通过 `printk_panic_text()` 直接写所有 active console，避免依赖异步线程；
- 打印 CPU id、当前任务信息（如果安全）；
- 进入无限循环/停机。

panic 路径必须容忍锁已经被持有、IRQ 状态异常、当前任务指针无效等情况。

## 4. 初始化顺序

```text
start_kernel:
  clocksource_init()
  rtc_init()
  timekeeper_init()
  timer_init()
  trap_init()

core_initcall:
  irq_init()
  plic_driver_register()

postcore_initcall:
  irqchip_init()
  irq_selftest()

subsys_initcall:
  设备驱动 request_irq()

start_kernel 末尾:
  irq_init_hart(boot_hart)
  local_irq_enable()

secondary_entry:
  timer_init_hart()
  trap_init()
  irq_init_hart(hart_id)
  local_irq_enable()
```

## 5. 锁与不变量

- `irq_desc.lock`：action 链表、depth、count、chip mask/unmask；
- `irq_alloc_lock`：descriptor 分配；
- `irq_domain_lock`：domain 链表；
- `irqchip_lock`：driver/controller 链表；
- `plic` 的 MMIO 访问由 descriptor lock 和调用上下文保护；
- `tk_lock`：jiffies 和墙钟偏移；
- `printk_ring.lock`：ring buffer 的 head/tail/reader cursor（irqsave）；
- console 有独立的输出锁；
- `ktime_nanosleep` 依赖 `task_sleep` 的条件锁协议。

## 6. 当前限制

- PLIC 假设 hart id 连续且等于 CPU id，`plic_ctx = 2 * hart + 1`；
- PLIC 所有映射使用 level-high 语义和 `handle_simple_irq`，没有完整 trigger 配置；
- IRQ domain revmap 无并发锁；
- `free_irq()` 不等待正在执行的 handler；
- timer tick 只在 boot hart 增加 jiffies，但所有 hart 都唤醒 `&jiffies`；
- jiffies 实际是 1 ms tick，不是传统 HZ 配置；
- `task_is_killed()` 语义过宽；
- printk 是固定 1024 字节 payload，超长消息截断；
- 没有动态 console 级别过滤；
- panic 停机而不是重启。
