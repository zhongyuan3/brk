# TODO — 设备与驱动框架重构

目标：把当前“DT 散点扫描 + 驱动内部主动扫描 + 部分直接 probe”的实现，逐步整理成统一的分层设备模型：

```text
DT 解析 → device_node / resource
        → 创建设备并挂到总线
        → 驱动注册
        → 总线 match
        → probe
```

总体方向合理，但需要把“设备创建”和“驱动 probe”拆开，并明确启动关键设备的早期依赖。

---

## 当前实现的主要问题

- [x] 已确认 `platform_bus`、`device`、`device_driver`、`bus_type` 已有最小模型
- [x] 早期 FDT 扫描已收敛到 DT/OF 核心层（irqchip 改用 OF tree）
- [x] 驱动内部反向调用 DT 扫描（`virtio_blk_init()` / `virtio_net_init()`）
- [x] irqchip 绕过 device/bus，直接扫描 DT 并 probe（已评估，保留为早期例外）
- [x] RTC 已去特殊化：platform driver probe 时注册 `wall_clock` provider
- [x] `platform_device` 只有一个 MMIO 窗口和一个 IRQ
- [x] `dt_get_reg()` 已删除，地址解析统一使用 `of_address_to_resource()`
- [x] 驱动直接依赖 libfdt / `dt_get_fdt()` / `dt_get_reg()`
  - [x] 普通驱动已改用 `device_node` / `resource` / `of_*`
  - [x] PLIC 作为 early class 例外保留 `dt_get_reg()`
- [x] virtio-mmio transport 的逻辑在 DT 层，virtio 设备没有 parent
- [x] virtio 设备顺序依赖反向注册 hack
- [x] `device_snapshot()` 保存裸指针，缺少设备引用和生命周期保护
- [x] 没有 `device_add()` / `device_del()` / release / deferred probe
- [x] 同 initcall level 的依赖已通过阶段拆分和 `of_root()` 保证消除

---

## 目标分层

```text
DT/OF 层
  device_node、property、resource、地址转换、IRQ spec

设备核心层
  device、driver、bus、match、probe、remove、refcount、release

总线层
  platform_bus、virtio_bus、irqchip early class

驱动层
  ns16550a、virtio_mmio、virtio_blk、virtio_net、goldfish_rtc、plic
```

---

## Phase 1 — OF / device_node / resource 抽象（已完成）

目标：先建立可供后续驱动使用的统一抽象，**不改变现有驱动的行为**。

- [x] 新增 `src/include/brk/resource.h`
  - [x] `struct resource`
  - [x] `IORESOURCE_MEM` / `IORESOURCE_IRQ`
- [x] 新增 `src/include/brk/of.h`
  - [x] `struct device_node`
  - [x] `of_init()`
  - [x] `of_root()`
  - [x] `of_find_node_by_path()`
  - [x] `of_find_compatible_node()`
  - [x] `of_find_node_by_phandle()`
  - [x] `of_get_parent()` / `of_get_child_by_name()`
  - [x] `of_get_property()` / `of_property_read_u32()` / `of_property_read_string()`
  - [x] `of_device_is_available()`
  - [x] `of_node_name()` / `of_node_full_name()`
  - [x] `of_address_to_resource()`
  - [x] `of_irq_get()`（先包装现有 `irq_of_parse_and_map()`）
- [x] 新增 `src/dev/dt/of.c`
  - [x] 从 FDT 构建 `device_node` 树
  - [x] 解析 name/full_name/device_type/phandle/parent/children
  - [x] 属性访问仍基于 FDT offset
  - [x] `ranges` 地址转换
  - [x] `status` 可用性判断
  - [x] `reg` 转 `struct resource`
  - [x] `of_irq_get()` 兼容现有 irqdomain
- [x] 初始化顺序
  - [x] `of_init()` 在 slab/kmalloc 可用后建立节点树
  - [x] 保持现有 `dt_init_scan_*`、`rtc_init()`、`irqchip_init()` 行为不变
- [x] 验证
  - [x] 内核全量编译通过
  - [x] QEMU 起机，串口、virtio、RTC、rootfs 行为不变
  - [x] 节点树构建失败时只告警，不阻断启动

## Phase 2 — 通用 platform device 枚举（已完成）

- [x] 实现 `of_platform_populate()`
  - [x] 跳过 `cpus` / `memory` / `chosen` / `reserved-memory` / `aliases`
  - [x] 只处理 `status = "okay"` 或没有 status 的节点
  - [x] 有 `compatible` 的节点创建 platform device
  - [x] `simple-bus` 递归
  - [x] `ranges` 地址转换
  - [x] 使用 `<node-name>@<unit-address>` 命名
- [x] `platform_device` 改为 resource 数组
- [x] 新增 `platform_get_resource()` / `platform_get_irq()`
- [x] 串口迁移
  - [x] 删除 `dt_init_scan_serial()`
  - [x] `ns16550a` 使用 platform resource 和 `of_node`
  - [x] console 注册时机保持可用
- [x] 验证串口、early printk、rootfs

## Phase 3 — 设备核心生命周期（已完成）

- [x] `device_initialize()` / `device_add()` / `device_del()`
- [x] `device_get()` / `device_put()` / `release()`
- [x] `device` 增加 refcount、state、per-device lock、deferred-probe 节点
- [x] `device_snapshot()` 返回带引用的设备，`device_snapshot_free(devices, count)` 负责释放
- [x] `driver_register()` 检查重复驱动（同实例 `-EBUSY`，同名 `-EEXIST`）
- [x] probe/remove 与设备注销的并发语义
  - [x] probe 期间持有设备引用
  - [x] `device_del()` 与正在运行的 probe 通过 state/driver 交接
  - [x] `driver_unregister()` 与正在运行的 probe 通过 `drv->registered` 交接
- [x] 增加 `-EPROBE_DEFER`、延迟 probe 队列和 `late_initcall` 重试
- [x] 验证设备注册/注销路径
  - [x] 新增 `src/dev/base/device_selftest.c`
  - [x] 覆盖 release、device_get/put、snapshot 引用、重复驱动、deferred probe
  - [x] QEMU 启动输出 `device-selftest: all tests passed`

说明：platform device 已改为由 `dev->release` 负责最终释放；tty、virtio、gendisk 仍由各自容器手动释放，后续迁移到 release 回调时再统一。

---

## Phase 4 — virtio transport 拆分（已完成）

- [x] 新增 `virtio_mmio` platform driver，匹配 `"virtio,mmio"`
  - [x] `src/dev/virtio/virtio_mmio_platform.c`
- [x] transport probe 负责：
  - [x] ioremap / 读取 device ID（`virtio_dev_create()`）
  - [x] 创建 child `virtio_device`
  - [x] 设置 parent
  - [x] 注册到 `virtio_bus`
- [x] `virtio_blk` / `virtio_net` 只注册到 `virtio_bus`
- [x] 删除 `dt_init_scan_virtio_dev()` 和驱动内部 DT 扫描
- [x] 删除反向注册 hack，改为稳定地址排序
  - [x] `of_platform_populate()` 按第一个 `reg` 地址排序子节点
  - [x] 地址相同时按节点完整路径排序
- [x] 验证 `/dev/virtio_blk0` 编号稳定、网络可用
  - [x] QEMU 中 virtio0 = bus 0，virtio_blk0 正常挂载 rootfs
  - [x] virtio1 = bus 1，网络正常
  - [x] `brktest all`: 10 suite(s), 0 failed

---

## Phase 5 — irqchip / 早期设备顺序（已完成）

- [x] 明确 `irqchip` 早期 probe 阶段
  - [x] `irqchip_early_init()` 在 `postcore_initcall` 执行
  - [x] `of_platform_populate()` 在 `subsys_initcall` 创建设备
- [x] IRQ controller 在普通设备 probe 前完成 domain 注册
- [x] 普通设备资源中的 IRQ 延迟到 probe 阶段解析
  - [x] `of_platform` 不再生成 IRQ resource
  - [x] `platform_get_irq()` 在 probe 时通过 `of_irq_get()` 解析
- [x] 支持 `interrupt-parent` 继承和 `interrupts-extended`
  - [x] `interrupt-parent` 从最近祖先继承
  - [x] `interrupts-extended` 按每个 entry 的 parent 解析
- [x] 评估是否将 irqchip 完全设备化；否则记录为明确例外
  - [x] 结论：保留 early irqchip 例外
  - [x] 原因：控制器自身需要 bootstrap IRQ，且必须早于普通消费者 probe
  - [x] 例外说明记录在 `src/kern/irqchip.c`
- [x] 验证 PLIC、串口、virtio、网络中断
  - [x] `/proc/interrupts`: `ns16550a` hwirq 10、`virtio_blk` hwirq 1 正常计数
  - [x] 自定义 DTB 验证串口 `interrupts-extended` 和 virtio 的 `interrupt-parent` 继承
  - [x] `brktest all`: 10 suite(s), 0 failed

---

## Phase 6 — 其余驱动迁移（已完成）

- [x] `goldfish_rtc`
  - [x] 保留 early wall-clock provider（timekeeper 依赖）
  - [x] 早期路径改用 `of_init()` / `of_find_compatible_node()` / `of_address_to_resource()`
  - [x] 删除 DT 层的 `dt_parse_rtc()`
  - [x] 增加 platform driver / device（匹配 `google,goldfish-rtc`）
  - [x] platform probe 复用早期已建立的 MMIO 映射
  - [x] 启动日志确认 `rtc: bound rtc@101000 at 0x101000`
  - [x] `date` 输出正确的 wall-clock 时间
- [x] `ns16550a` 完全迁移到 platform bus
  - [x] 仅使用 `platform_get_resource()` / `platform_get_irq()` / `of_property_read_u32()`
- [x] `plic` 迁移或明确 early class
  - [x] 明确保留为 `irqchip_early_init()` 早期例外
- [x] `genhd` / `devtmpfs` 设备注册路径检查
  - [x] `devtmpfs_populate()` 使用带引用的设备快照
  - [x] 磁盘、分区在 `device_register()` 前设置 `devt` / `mode`
  - [x] `del_gendisk()` 先注销分区和磁盘设备，再释放容器
  - [x] 启动验证 `/dev/virtio_blk0` 与分区节点存在
- [x] 删除 DT 层所有驱动专属 compatible 字符串
  - [x] `google,goldfish-rtc` 已移入 RTC 驱动
  - [x] `virtio,mmio` 已在 Phase 4 移入 transport driver
  - [x] DT 层仅保留通用的 `simple-bus` 判断

---

## Phase 7 — 清理与回归（已完成）

- [x] 统一 initcall 阶段
  - [x] early：memory/cpu/timebase，显式在 `start_kernel()`/`arch_init()` 中完成
  - [x] core：device/bus/driver/irq core、slab cache、net core
  - [x] postcore：device_node、irqchip、platform populate
  - [x] subsys：transport/platform drivers（ns16550a、goldfish RTC、virtio-mmio）
  - [x] device：叶子驱动（virtio-blk、virtio-net）
  - [x] late：deferred probe 重试、自检
  - [x] 块分区扫描/文件系统放在 `do_initcalls()` 之后的任务上下文中（需要真实 I/O 和调度）
- [x] 驱动不再直接调用 `fdt_*` / `dt_get_reg()`
  - [x] PLIC 改用 `of_address_to_resource()` / `of_property_read_u32()`
  - [x] `dt_get_reg()` 已删除
  - [x] libfdt 只保留在 DT/OF 核心层
- [x] 更新 README / 架构文档
  - [x] 增加设备模型、virtio 分层和 initcall 阶段说明
  - [x] 记录 IRQ/RTC/clocksource/console 早期例外
- [x] `make -j`、`make run`
- [x] brktest 全量：10 suite(s), 0 failed
- [x] QEMU 串口、RTC、块设备、网络、rootfs 回归

---

## 关键原则

- 设备创建和驱动 probe 必须分离
- DT 层不能知道具体驱动
- 驱动不能主动扫描 FDT
- IRQ domain 必须先于消费者 probe
- 驱动 probe 不在全局 spinlock 内执行
- 设备快照必须有生命周期保护
- 早期 console/clocksource 可以作为明确记录的例外

---

## 验收标准

- [x] DT 层不再出现 `"ns16550a"`、`"virtio,mmio"`、`"google,goldfish-rtc"` 等驱动专属字符串
- [x] 驱动只使用 `device_node` / `resource` / `of_*`
- [x] 普通设备统一走 bus match + probe
- [x] virtio-mmio 是 platform driver，virtio_blk/net 是 virtio bus driver
- [x] IRQ controller 在普通设备 probe 前就绪
- [x] `/dev/virtio_blk0` 编号稳定
- [x] 启动、串口、RTC、块设备、网络、rootfs 全部回归通过

---

# 时间子系统重构方案（T1/T2 已完成，T3–T5 待实施）

## 现有问题

- `rtc_init()` 在 `start_kernel()` 中、所有 initcall 之前被直接调用，RTC 被迫成为特殊设备
- `timekeeper_init()` 直接调用 `rtc_is_available()` / `rtc_read_ns()`
- `timekeeper_set_real()` 直接调用 `rtc_set_ns()`，时间核心反向依赖 RTC 驱动
- `rtc_*` 是全局单例 API，驱动只负责“接管”早期已经映射好的 RTC
- RTC 早期路径需要提前调用 `of_init()`，把 OF 初始化也拖到了 initcall 之前
- clocksource 只有全局 timebase frequency，没有 `struct clocksource` 抽象
- timekeeper 直接调用 `timer_read()`，与 RISC-V timer 强耦合
- 每个 tick 由 arch timer 直接调用 `timekeeper_tick()`、`tcp_timer_tick()`、`task_wake_all()`
- `timekeeper_set_real()` 无法把 RTC 写失败返回给 `settimeofday()` / `clock_settime()`
- `CLOCK_MONOTONIC` / `CLOCK_REALTIME` / `CLOCK_GETRES` 的分辨率和语义仍比较粗糙

## 目标分层

```text
clocksource（单调 cycle counter）
        │
        ▼
timekeeper（monotonic ns、wall offset、jiffies）
        │
        ├── clockevent/tick
        └── wall_clock provider（RTC / 其他 wall clock）
                │
                ▼
        goldfish RTC platform driver
```

关键原则：

- timekeeper 必须先于 initcalls 启动，但**不能依赖 RTC 已经 probe**
- RTC 是普通 platform driver，probe 成功后注册 `wall_clock`
- `wall_clock` 注册时，timekeeper 才读取 RTC 并建立 wall offset
- `settimeofday()` 更新 offset，并可选地把时间写回当前 provider
- clocksource 与 wall clock 完全解耦
- 没有 RTC 时，系统仍能启动，monotonic 时间正常，`CLOCK_REALTIME` 从 0 附近开始
- 早期 clocksource/timebase 仍可作为明确记录的早期例外

## Phase T1 — wall_clock 抽象与 RTC 去特殊化（已完成）

实施状态：

- [x] `struct wall_clock` / `wall_clock_ops`
- [x] `timekeeper_register_wall_clock()` / `timekeeper_unregister_wall_clock()`
- [x] `timekeeper_init()` 不再读取 RTC，wall offset 从 0 开始
- [x] RTC probe 时注册 wall clock 并建立 wall offset
- [x] `timekeeper_set_real()` / `ktime_set_real()` 返回错误码
- [x] `settimeofday()` / `clock_settime()` 传播 provider 写错误
- [x] RTC 只作为普通 platform driver
- [x] 删除 `rtc_init()`、`src/riscv/kern/rtc.c`、全局 `rtc_*` API
- [x] 验证：
  - [x] 有 RTC：启动日志 `wall clock goldfish-rtc registered`，`date` 正确
  - [x] 无 RTC：正常启动，realtime 从 0 附近开始
  - [x] `settimeofday()` 往返测试通过（临时用户态测试程序）
  - [x] `brktest all` 通过

### 新增 wall clock 接口

建议放在 `src/include/brk/timekeeper.h`：

```c
struct wall_clock;

struct wall_clock_ops {
	int (*read_ns)(struct wall_clock *wc, u64 *ns);
	int (*set_ns)(struct wall_clock *wc, u64 ns);
};

struct wall_clock {
	const char *name;
	const struct wall_clock_ops *ops;
};

int timekeeper_register_wall_clock(struct wall_clock *wc);
void timekeeper_unregister_wall_clock(struct wall_clock *wc);
```

### timekeeper 改动

- `timekeeper_init()`
  - 不再调用 `rtc_*`
  - 初始化 `wall_offset_ns = 0`
  - 记录 `mono_base`
  - `wall_valid = false`
- 新增内部状态：
  - `struct wall_clock *wall_clock`
  - `bool wall_valid`
- `timekeeper_register_wall_clock()`
  - 读取 provider 时间
  - 若当前 wall 尚未被设置，建立 `wall_offset_ns = rtc_ns - mono_ns`
  - 若用户已经 `settimeofday()`，不覆盖用户设置
  - 同一时间只允许一个 active provider（或记录 owner）
- `timekeeper_set_real(ns)`
  - 更新 `wall_offset_ns`
  - 标记 `wall_valid = true`
  - 若存在 provider，调用 `provider->ops->set_ns()`
  - 建议改为返回 `int`，把写失败传播给 syscall
- `timekeeper_unregister_wall_clock()`
  - 清空 provider
  - 保留当前 wall offset

### RTC driver 改动

`src/dev/rtc/google_goldfish_rtc.c`：

- 删除早期 `goldfish_rtc_early_probe()` 和全局 `rtc_ok`
- probe 时：
  - 从 platform resource 取 MMIO
  - `ioremap`
  - 初始化 `struct goldfish_rtc`
  - 填充 `struct wall_clock`
  - `timekeeper_register_wall_clock()`
- remove 时：
  - `timekeeper_unregister_wall_clock()`
  - `iounmap`
  - 释放 RTC 私有数据
- RTC 读写只通过 `wall_clock_ops` 暴露给时间核心

### 删除的特殊路径

- 删除 `src/riscv/kern/rtc.c`
- 从 `start_kernel()` 删除 `rtc_init()`
- 删除 `brk/rtc.h` 中的全局 `rtc_*` API
- 删除 `goldfish_rtc.h` 中的 early probe 声明
- RTC 不再需要提前调用 `of_init()`
- `of_init` 回归普通 `postcore_initcall`

### 启动顺序

```text
start_kernel
  clocksource_init()
  timekeeper_init()          # wall = 0, mono base
  timer_init()
  do_initcalls()
    postcore: of_init / irqchip / of_platform
    subsys:   goldfish RTC probe
                -> timekeeper_register_wall_clock()
                -> wall offset corrected
  fs_init()                  # 之后的文件时间已经是真实 wall time
```

### 验证

- 正常启动后 `date` 正确
- `settimeofday()` / `clock_settime(CLOCK_REALTIME)` 后 `gettimeofday()` 正确
- 有 RTC 时 `settimeofday()` 可写回硬件
- 临时移除 DTB 中 RTC 节点后：
  - 内核仍能启动
  - monotonic / nanosleep 正常
  - `CLOCK_REALTIME` 从 0 附近开始
- `brktest all` 10 suite(s), 0 failed

## Phase T2 — clocksource 抽象（已完成）

实施状态：

- [x] `struct clocksource`：`name` / `read` / `frequency` / `rating` / `list`
- [x] `clocksource_register()` / `clocksource_select()`
- [x] RISC-V time CSR 注册为 `riscv-time` clocksource
- [x] `timer_clocksource_init()` 读取 timebase-frequency 并注册
- [x] `clocksource_init()` 选择 rating 最高的 clocksource
- [x] timekeeper 不再直接调用 `timer_read()`
- [x] `timekeeper_mono_ns()` 基于 `clocksource_read_ns()`
- [x] `timer_init()` 使用选中 clocksource 的 frequency
- [x] `dt_setup_timebase_freq()` 改为 `dt_get_timebase_freq()`
- [x] 验证：
  - [x] 启动日志 `clocksource: selected riscv-time (10000000 Hz, rating 100)`
  - [x] `date`、`/proc/uptime` 正常
  - [x] 无 RTC 启动正常
  - [x] `settimeofday()` 往返测试通过
  - [x] `brktest all` 通过

### 设计

- 新增 `struct clocksource`：

```c
struct clocksource {
	const char *name;
	u64 (*read)(void);
	u32 frequency;
	u32 rating;
	struct list_head list;
};
```

### 设计落点

- `clocksource_register()` / `clocksource_select()` 实现在 `src/kern/clocksource.c`
- RISC-V time CSR 由 `src/riscv/kern/timer.c` 注册为 arch clocksource
- timekeeper 只消费 `clocksource_read_ns()`
- `timer_init()` 通过选中 clocksource 的 frequency 计算 tick interval
- timebase-frequency 暂时保留为早期 DT 例外，通过 `dt_get_timebase_freq()` 提供
- `timer_read()` 仍保留给 arch timer 编程和随机数种子，不再参与 timekeeper

## Phase T3 — clockevent / tick 解耦

- 定义 tick 回调注册机制，例如：
  - `timer_tick_register(struct timer_tick *tt)`
  - `timer_tick_unregister()`
- `timekeeper_tick()` 只负责 jiffies 和等待队列唤醒
- arch `handle_tick()` 不再直接调用网络/任务代码
- 将 TCP timer tick 等周期性工作移到注册回调
- 明确 `HZ` / tick period 常量，统一 jiffies 与用户态 clock tick 换算

验证：

- 网络重传、`select`、`nanosleep`、调度时间统计正常
- `/proc/uptime`、`times()`、`getrusage()` 一致

## Phase T4 — POSIX 时间语义与错误传播

- `timekeeper_set_real()` / `ktime_set_real()` 返回错误码
- `settimeofday()` / `clock_settime()` 传播 RTC 写失败
- `clock_getres()` 使用 clocksource / wall clock 的实际分辨率
- 明确：
  - `CLOCK_REALTIME` = monotonic + wall offset
  - `CLOCK_MONOTONIC` = 不受 wall 调整影响
  - `CLOCK_PROCESS_CPUTIME_ID` / `CLOCK_THREAD_CPUTIME_ID`
- 评估是否增加 `CLOCK_BOOTTIME`
- 检查 `sys_times()` 的 `USER_HZ` 与内核 `HZ` 关系

## Phase T5 — 文档与回归

- README 增加时间子系统分层图
- 增加时间 selftest（建议覆盖）：
  - monotonic 单调性
  - wall clock 注册/注销
  - 用户 set time 不被后注册的 RTC 覆盖
  - 无 wall clock 时的行为
  - provider read/set 回调调用次数
- 全量回归：
  - debug / release 编译
  - QEMU 起机
  - 串口、RTC、块设备、网络、rootfs
  - `brktest all`

## 待决策

- 多个 RTC/wall clock 的优先级：第一个注册者优先，还是按 rating 选择？
- `settimeofday()` 时 provider 写失败是否返回错误，还是只告警？
- wall clock 注册是否总是覆盖 boot 初始值，还是只在用户未设置时覆盖？
- clocksource 是否引入 `mult/shift`，还是继续使用整数 frequency？
- timebase-frequency 继续作为早期 DT 例外，还是迁移到 clocksource driver？
