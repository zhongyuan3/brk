# 设备模型、块层与驱动

本文描述 BRK 的设备模型、设备号分配、块层、virtio 传输、virtio-blk、TTY/串口、RTC 和设备树扫描。IRQ 框架和 PLIC 的细节见 [irq_time.md](irq_time.md)。

相关源码：

- `src/include/brk/device_model.h`、`device.h`、`devno.h`、`chrdev.h`、`blkdev.h`、`bio.h`、`genhd.h`
- `src/include/brk/virtio.h`、`virtio_regs.h`、`virtio_queue.h`、`virtio_blk.h`、`virtio_net.h`
- `src/include/brk/tty.h`
- `src/driver/base/*.c`
- `src/driver/block/virtio_blk.c`
- `src/driver/char/ns16550a.c`
- `src/driver/tty/*.c`
- `src/driver/virtio/*.c`
- `src/driver/dt/dt.c`
- `src/driver/rtc/google_goldfish_rtc.c`
- `src/driver/irqchip/sifive_plic.c`

## 1. 设备模型

### 1.1 核心对象

```c
struct bus_type {
    const char *name;
    bool (*match)(struct device *, struct device_driver *);
    struct list_head devices;
    struct list_head drivers;
    struct list_head list;
};

struct device {
    char name[...];
    struct device *parent;
    struct bus_type *bus;
    struct device_driver *driver;
    struct list_head bus_node;
    struct list_head global_node;
    struct list_head child_node;
    struct list_head children;
    void *platform_data;
    void *driver_data;
    dev_t devt;
    umode_t mode;
    bool registered;
};

struct device_driver {
    const char *name;
    struct bus_type *bus;
    const struct device_id *id_table;
    int (*probe)(struct device *);
    int (*remove)(struct device *);
    struct list_head bus_node;
    ...
};
```

全局状态：

- `bus_list`
- `device_list`
- `device_lock`

### 1.2 注册与探测

`bus_register()`：

- 把 bus 加入全局链表。

`device_register(dev)`：

1. 拒绝重复注册；
2. 加入全局 device 链表；
3. 加入 parent 的 children；
4. 加入 bus 的 device 链表；
5. 如果 `devt != 0`，通知 devtmpfs；
6. 调用 `device_probe(dev)`。

`device_probe()`：

- 需要 `bus->match`；
- 扫描该 bus 上的 driver；
- 第一个匹配的 driver 绑定 `dev->driver`；
- 释放 `device_lock` 后调用 `driver->probe()`；
- probe 失败则清除绑定。

`driver_register()`：

- 加入 bus 的 driver 链表；
- 对已有同 bus 设备逐个 probe。

`device_unregister()`：

- 调用 remove；
- 从所有链表移除；
- 通知 devtmpfs 删除节点。

### 1.3 Platform bus

- 匹配方式：`platform_device.compatible` 与 driver `id_table[].compatible`；
- `platform_device_register_simple()` 创建设备；
- 设备树扫描器把 UART 等节点转换成 platform device。

### 1.4 Virtio bus

- 匹配方式：`virtio_device.id` 与 driver `device_id`；
- `virtio_dev_create(phys_base, size, irq)`：
  - 分配 `virtio_device`；
  - `ioremap` MMIO 区域；
  - 探测 virtio MMIO 版本；
  - 读取 device ID；
  - 命名为 `virtioN`；
  - 设置 bus。
- `virtio_dev_add()` 加入 vdev 链表并调用 `device_register()`；
- `virtio_dev_get(id)` 返回第一个匹配设备，当前不增加引用。

## 2. 设备号与字符设备

### 2.1 设备号编码

`dev_t`：

```text
bits 28..31 : type (CHRDEV=0, BLKDEV=1<<28)
bits 20..27 : major (8 bits)
bits  0..19 : minor (20 bits)
```

宏：

- `MKDEV`
- `MKCHRDEV`
- `MKBLKDEV`
- `MAJOR` / `MINOR` / `DEVTYPE`
- `IS_CHRDEV` / `IS_BLKDEV`

静态主设备号：

- `NS16550A_MAJOR = 1`
- `VIRTIO_BLK_MAJOR = 1`
- 动态 major 从 16 开始。

### 2.2 devno 分配器

`struct devno_allocator`：

- major 位图；
- 每个 major 的已分配 minor region 排序链表。

`devno_alloc_region()`：

- major 0 表示动态分配 major；
- 指定 major 必须已经保留/分配；
- 从 `base_minor` 开始找第一个足够大的空洞；
- 返回 `MKDEV(major, minor)`，type 由调用者附加。

`devno_free_region()` 要求精确匹配 region。

### 2.3 字符设备

`struct char_dev`：

- `dev_t`
- 驱动 `fops`
- hash node
- refcount
- dead flag

`chrdev_register()`：

- 要求字符类型 dev_t；
- 驱动 `open` 非空；
- minor region 已分配；
- 插入哈希表。

`chrdev_open()`：

- 按 `inode->rdev` 查找 cdev；
- 设置 `file->ops = &chrdev_fops`；
- `file->dev_private = cdev`；
- 调用驱动 `open()`。

`chrdev_fops` 是转发层：所有操作从 `file->dev_private` 找 cdev，再调用驱动 `fops`。驱动应把每次打开的状态放在 `file->private_data`。

## 3. 块层

### 3.1 `struct bio`

```c
struct bio {
    struct block_dev *bi_bdev;
    u64 bi_sector;          /* 在该 bdev 视图内的 512 字节扇区 */
    u64 bi_disk_sector;     /* 翻译后的整盘扇区 */
    size_t bi_size;
    struct bio_vec bi_vec[BIO_MAX_VECS];
    int bi_op;
    int bi_status;
    bool bi_done;
    struct bio *bi_next;
};
```

- `BIO_MAX_VECS = 8`
- 操作：`REQ_OP_READ`、`REQ_OP_WRITE`、`REQ_OP_FLUSH`
- 向量缓冲区是内核线性映射地址；
- 每个向量长度必须是 512 的倍数。

### 3.2 Request queue

`struct request_queue`：

- spinlock
- worker sleeplock
- 请求链表
- `queue_rq` 回调
- 所属 gendisk
- worker task
- dead/worker_done 标志

`bio_lookup_queue()`：

- 沿 `bd_parent` 链累加 `bd_start_sect`；
- 找到整盘的 `bd_queue`；
- 返回 queue 和整盘扇区。

`submit_bio_wait()`：

1. 校验大小/对齐/边界；
2. 找到 queue；
3. 懒创建 worker kthread；
4. 创建 request；
5. 如果与上一个请求在扇区上连续且操作相同，合并；
6. 否则入队并唤醒 worker；
7. 在 bio 上睡眠直到完成。

`blk_queue_worker()`：

- 逐个取 request；
- 同步调用 `q->queue_rq(q, rq)`；
- 标记所有 bio 完成并唤醒；
- queue 销毁时把所有未完成 bio 以 `-ENODEV` 完成。

### 3.3 `block_dev`

```c
struct block_dev {
    dev_t dev;
    unsigned int phy_bsize;
    u64 phy_bcnt;           /* 512 字节扇区数 */
    struct page_cache *bd_mapping;
    ...
    struct block_dev *bd_parent;  /* 分区视图 */
    u64 bd_start_sect;            /* 分区在父设备中的起始扇区 */
    struct request_queue *bd_queue; /* 仅整盘 */
};
```

- `blkdev_alloc()` 创建设备页缓存；
- `blkdev_register()` 要求块类型 dev_t 且 minor region 已分配；
- `blkdev_get/put()` 管理引用；
- `blkdev_read/write()` 以 512 字节扇区为单位；
- `blkdev_flush()` 提交空 `REQ_OP_FLUSH` bio。

### 3.4 块设备页缓存

`bdev_pc_ops`：

- `read_page`：把页索引映射到扇区范围，短尾页填零；
- `write_page`：只写设备容量内的扇区；
- 要求 `phy_bsize <= PAGE_SIZE` 且能整除页大小。

### 3.5 原始块设备文件

`blkdev_fops`：

- `open`：`blkdev_get(inode->rdev)`，把 bdev 存入 `file->private_data`；
- `release`：刷页缓存和设备，`blkdev_put()`；
- `read/write`：按字节偏移访问 bdev 页缓存，使用 `copy_to/from_user`；
- `llseek`：SET/CUR/END；
- `fsync`：刷页缓存 + `blkdev_flush`；
- `ioctl`：`BLKGETSIZE64`、`BLKFLSBUF`。

### 3.6 gendisk 与分区

- `GENHD_MAX_PARTITIONS = 4`
- `GENHD_MINORS = 5`（整盘 + 4 个主分区）
- MBR 解析：签名 `0xAA55`，4 个主分区；
- GPT protective type `0xEE` 能识别但不解析 GPT。

`gendisk_alloc()`：

- 分配 gendisk + 整盘 block_dev；
- 初始化 device model 项；
- 设置 bdev dev_t、`phy_bsize=512`、`phy_bcnt`。

`add_disk()`：

- 注册整盘 bdev；
- 注册设备节点（`S_IFBLK|0666`）；
- 加入全局 disk 链表。

`gendisk_scan_partitions()`：

- 快照未扫描磁盘；
- 在锁外读取 sector 0；
- 为每个有效主分区创建 block_dev：
  - 同 major，minor = `first_minor + partno`；
  - `bd_parent` 指向整盘并增加引用；
  - `bd_start_sect` 为分区起点；
  - 注册 bdev 和设备节点。

## 4. Virtio

### 4.1 MMIO 传输

`virtio_mmio.c` 支持 version 2：

| 寄存器 | 偏移 |
| --- | --- |
| magic | `0x000` |
| version | `0x004` |
| device ID | `0x008` |
| vendor ID | `0x00c` |
| device features | `0x010` |
| driver features | `0x020` |
| queue select | `0x030` |
| queue max size | `0x034` |
| queue size | `0x038` |
| queue ready | `0x044` |
| queue notify | `0x050` |
| interrupt status | `0x060` |
| interrupt ack | `0x064` |
| status | `0x070` |
| desc/avail/used 物理地址 | `0x080/0x090/0x0a0` |
| config space | `0x100` |

关键限制：

- 只处理 32 位 feature 低字；
- queue setup 要求设备支持请求的 queue 大小；
- queue 已 ready 时 setup 会 panic。

### 4.2 Virtqueue

split virtqueue 结构：

- `virtq_desc`：addr/len/flags/next；
- `virtq_avail`：flags/idx/ring；
- `virtq_used_elem`：id/len；
- `virtq_used`：flags/idx/ring。

描述符分配：

- 线性扫描 `desc_used`；
- 链通过 `VIRTQ_DESC_F_NEXT` 连接；
- `virtq_submit()` 写 avail ring，内存屏障后递增 `avail->idx`；
- `virtq_process_used()` 消费 used ring 并回调。

### 4.3 virtio-blk

- device ID 2；
- 请求头：

```c
struct virtio_blk_req {
    u32 type;      /* IN/OUT/FLUSH */
    u32 reserved;
    u64 sector;
};
```

请求路径：

1. 获取 `vblk->lock`；
2. 分配 descriptor chain：header + data vectors + status；
3. 提交并 notify queue 0；
4. 在 transaction 上睡眠等待 IRQ；
5. IRQ 处理 used ring，标记完成并唤醒；
6. 转换设备状态码。

初始化：

- 校验 device ID；
- 校验 queue size 是 2 的幂；
- reset、ACK/DRIVER；
- 协商 features（清除 RO/SCSI/WCE/MQ/ANY_LAYOUT/EVENT_IDX/INDIRECT_DESC，保留 FLUSH）；
- FEATURES_OK；
- setup queue 0；
- DRIVER_OK；
- 读取容量；
- `request_irq`。

注册：

- 最多 4 个设备；
- 名称 `virtio_blkN`；
- block major 1，每个设备 5 个 minor；
- `blk_init_queue()` + `add_disk()`。

**已知风险**：`virtio_blk_submit_vec()` 允许 16 个数据向量，但队列大小为 16，header+status 需要额外两个描述符；16 段请求可能因无法分配完整链而永久睡眠。

### 4.4 virtio-net

细节见 [networking.md](networking.md)。

## 5. TTY 与串口

### 5.1 TTY 对象

```c
struct tty_port {
    struct tty *tty;
    struct tty_driver *driver;
    spinlock_t lock;
    void *client_data;
    pid_t sid;
    pid_t pgrp;
    int open_count;
    struct termios termios;
    bool dead;
};

struct tty_driver {
    const char *name;
    struct tty_ops *ops;
    struct tty_port **ports;
    struct char_dev **cds;
    struct device **devs;
    ...
};

struct tty {
    struct tty_port *port;
    char rx_buf[1024];
    ...
    struct tty_ldisc_ops *ldisc;
    spinlock_t read_lock;
    spinlock_t write_lock;
};
```

### 5.2 TTY driver 注册

`tty_register_driver()`：

- major 0 时动态分配字符设备 region；
- 加入全局 `tty_driver_list`。

`tty_driver_add_port()`：

- 分配 `char_dev`；
- 创建设备节点 `tty<slot>`；
- 注册 cdev 和 device；
- 关联 `tty_port` 和 `tty`。

### 5.3 打开/关闭

`tty_open()`：

- 第一个 session leader 打开未关联终端时，使其成为 controlling terminal；
- 设置 foreground pgrp；
- 增加 tty refcnt 和 open_count。

`tty_close()`：

- 减少 open_count；
- 释放 tty 引用。

### 5.4 n_tty 行规程

默认 termios：

```text
iflag: ICRNL | IXON
oflag: OPOST | ONLCR
cflag: CS8 | CREAD | CLOCAL
lflag: ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN
```

输入处理：

- IGNCR/ICRNL/INLCR；
- VLNEXT；
- IXON；
- VDISCARD；
- ISIG：VINTR/VQUIT/VSUSP 发送信号；
- canonical：VERASE/VKILL/VWERASE/VREPRINT/VEOF；
- 行提交条件：换行、VEOL/VEOL2、缓冲区满。

读：

- canonical 模式一次最多返回一行；
- noncanonical 支持 MIN/TIME；
- 后台读发送 SIGTTIN；
- 信号中断返回 `-EINTR`。

写：

- 后台写发送 SIGTTOU（TOSTOP）；
- IXON 停止时等待；
- ONLCR 展开；
- FLUSHO 抑制输出。

### 5.5 ns16550a

- 注册字符 major 1，4 个端口；
- subsys initcall 注册 tty driver 和 platform driver；
- probe：
  - 从 platform data 获取 MMIO 地址、大小、IRQ、时钟；
  - `ioremap`；
  - 初始化 UART：38.4k 分频、8N1、FIFO、RX 中断；
  - `request_irq`；
- 第一个端口注册 boot console；
- IRQ handler 读一个字节并调用 `tty_receive()`；
- `put_char` 在设备锁下写 UART；
- devtmpfs 根据 device model 自动创建 `/dev/tty0..tty3`。

## 6. RTC 与设备树

### 6.1 RTC

- `rtc_init()` 在 `start_kernel()` 中直接调用；
- 查找 compatible `"google,goldfish-rtc"`；
- 获取 `reg` 物理地址/大小；
- `ioremap`；
- Goldfish RTC 两个 32 位寄存器：时间低/高；
- `rtc_read_ns` / `rtc_set_ns` 在 RTC spinlock 下访问；
- `timekeeper_init()` 读取 RTC 建立墙钟偏移；
- RTC 不存在时墙钟从 0 开始。

### 6.2 设备树扫描

`src/driver/dt/dt.c`：

- `dt_set_fdt()` 保存 FDT 指针；
- `dt_init_scan_cpu()` 扫描 `/cpus`，统计 `nr_harts`；
- `dt_init_scan_mem()` 读取 `/memory`；
- `dt_init_scan_rsvd_mem()` 读取 `/reserved-memory`；
- `dt_init_scan_serial()` 扫描 `/soc` 下 `ns16550a`，注册 platform device；
- `dt_init_scan_virtio_dev()` 扫描 virtio-mmio 节点，创建 virtio device；
- 读取中断信息时使用 `interrupt-parent`、`#interrupt-cells`、`interrupts`，最终走 IRQ domain。

## 7. 初始化顺序

```text
core_initcalls:
  blkdev_registry_init
  chrdev_registry_init
  platform_bus_init
  virtio_bus_init
  plic_driver_register

postcore_initcalls:
  irqchip_init
  irq_selftest

subsys_initcalls:
  dt_init_scan_serial
  virtio_blk_init
  virtio_net_init
  ns16550a_driver_init

CPU0 静态 init_task:
  fs_init()
    ├─ gendisk_scan_partitions()
    ├─ mount brkfs
    ├─ devtmpfs_populate()
    └─ 打开 tty0
```

依赖关系：

- `platform_bus_init` 必须先于串口 platform device/driver 注册；
- `virtio_bus_init` 必须先于 virtio 设备扫描；
- `irqchip_init` 必须先于设备驱动 `request_irq`；
- `blkdev_registry_init`/`chrdev_registry_init` 必须先于设备号分配；
- `gendisk_scan_partitions()` 需要块层和 virtio-blk 就绪；
- `devtmpfs_populate()` 在 brkfs 挂载前创建块设备节点，之后在 `/dev` 挂载后再 populate 一次。

## 8. 锁与不变量

- `device_lock`：全局设备/驱动链表和 probe 绑定；
- `bus` 的 device/driver 链表由 `device_lock` 保护；
- `cd_htable_lock`：字符设备哈希；
- `blkdev` 注册表锁：块设备哈希；
- `request_queue.lock`：请求队列和 bio 完成状态；
- `block_dev` 页缓存由页缓存锁规则保护；
- virtio：
  - `virtio_mmio` 寄存器访问由驱动自己的锁/调用上下文保护；
  - `virtio_blk.lock` 保护 descriptor slot 和完成状态；
  - `virtio_net.lock` 保护 RX/TX virtqueue；
- TTY：
  - `port->lock` 保护输入环、termios、pgrp、open_count；
  - `tty->read_lock` 串行化读；
  - `tty->write_lock` 串行化写；
- RTC 使用自己的 spinlock；
- 设备驱动 probe 在进程上下文，可以睡眠/ioremap/request_irq。

## 9. 当前限制

- 设备模型没有完整引用计数，设备/驱动移除与并发访问缺少同步等待；
- `device_snapshot()` 返回未加引用的指针，调用者必须保证设备不消失；
- 块层 worker 每队列一个线程，请求合并只做相邻同操作；
- virtio-blk 队列大小与最大向量数的边界存在风险；
- 没有 SCSI/NVMe/GPT 解析；
- TTY 只支持 n_tty，没有 line discipline 切换；
- 串口驱动只处理单字节 RX；
- 没有电源管理、热插拔、DMA API、IOMMU；
- 设备树解析是只读、启动期一次性扫描。
