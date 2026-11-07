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
- [ ] 早期 FDT 扫描仍保留在 `irqchip_early_init()`
- [x] 驱动内部反向调用 DT 扫描（`virtio_blk_init()` / `virtio_net_init()`）
- [x] irqchip 绕过 device/bus，直接扫描 DT 并 probe（已评估，保留为早期例外）
- [x] RTC 在 initcall 之前初始化（已评估，保留为早期例外，改用 OF API）
- [x] `platform_device` 只有一个 MMIO 窗口和一个 IRQ
- [ ] `dt_get_reg()` 没有 `ranges` 地址转换，也没有多 reg 支持
- [x] 驱动直接依赖 libfdt / `dt_get_fdt()` / `dt_get_reg()`
  - [x] 普通驱动已改用 `device_node` / `resource` / `of_*`
  - [x] PLIC 作为 early class 例外保留 `dt_get_reg()`
- [x] virtio-mmio transport 的逻辑在 DT 层，virtio 设备没有 parent
- [x] virtio 设备顺序依赖反向注册 hack
- [x] `device_snapshot()` 保存裸指针，缺少设备引用和生命周期保护
- [x] 没有 `device_add()` / `device_del()` / release / deferred probe
- [ ] 同 initcall level 的依赖主要依赖链接顺序

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

## Phase 7 — 清理与回归

- [ ] 统一 initcall 阶段
  - [ ] early：memory/cpu/timebase
  - [ ] core：device/bus/driver/irq core
  - [ ] postcore：device_node、irqchip、platform populate
  - [ ] subsys：transport/platform drivers
  - [ ] device：叶子驱动
  - [ ] late：块分区扫描/文件系统
- [ ] 驱动不再直接调用 `fdt_*` / `dt_get_reg()`
- [ ] 更新 README / 架构文档
- [ ] `make -j`、`make run`
- [ ] brktest 全量
- [ ] QEMU 串口、RTC、块设备、网络、rootfs 回归

---

## 关键原则

- 设备创建和驱动 probe 必须分离
- DT 层不能知道具体驱动
- 驱动不能主动扫描 FDT
- IRQ domain 必须先于消费者 probe
- 驱动 probe 不在全局 spinlock 内执行
- 设备快照必须有生命周期保护
- 早期 console/RTC/clocksource 可以作为明确记录的例外

---

## 验收标准

- [ ] DT 层不再出现 `"ns16550a"`、`"virtio,mmio"`、`"google,goldfish-rtc"` 等驱动专属字符串
- [ ] 驱动只使用 `device_node` / `resource` / `of_*`
- [ ] 普通设备统一走 bus match + probe
- [ ] virtio-mmio 是 platform driver，virtio_blk/net 是 virtio bus driver
- [ ] IRQ controller 在普通设备 probe 前就绪
- [ ] `/dev/virtio_blk0` 编号稳定
- [ ] 启动、串口、RTC、块设备、网络、rootfs 全部回归通过
