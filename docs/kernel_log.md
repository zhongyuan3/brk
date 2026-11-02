# BRK 内核日志系统

本文描述 `printk` 日志的存储格式、环形缓冲区、writer/reader 协议、覆盖检测、并发模型和 console/klogd 的消费方式。

## 1. 总体结构

```text
printk / pr_* / panic
        │
        ▼
  printk_log_write()          写入 ring buffer
        │
        ▼
  printk_ring_buffer          head / tail / records
        │
        ├── console reader ──► UART / tty console
        └── klogd reader   ──► /var/log/kernel.log
```

设计目标：

- `printk` 只负责格式化、写入 ring，不在锁内做慢速 console/磁盘 I/O；
- 每个 console 和 klogd 拥有独立的 reader 和 cursor；
- writer 覆盖旧记录时，reader 能准确知道丢失了多少数据；
- 支持 SMP、中断上下文和 panic 路径；
- 记录不跨环形边界，避免 reader 重组和边界条件。

## 2. 日志记录

### 2.1 记录格式

```text
| header | payload | padding |
```

`payload` 长度为 `len` 字节，不要求 NUL 结尾。记录总长度按记录头对齐要求向上取整：

```c
static inline u32 printk_record_total(u32 len)
{
	return round_up(sizeof(struct printk_log_header) + len,
			alignof(struct printk_log_header));
}
```

### 2.2 记录头

```c
#define PRINTK_LOG_MAGIC0 0x5a
#define PRINTK_LOG_MAGIC1 0xa5

struct printk_log_header {
	u8 magic[2];
	u8 level;
	u8 flags;
	u32 len;
	u64 nsec;
};

_Static_assert(sizeof(struct printk_log_header) == 16, ...);
_Static_assert(offsetof(struct printk_log_header, len) == 4, ...);
_Static_assert(offsetof(struct printk_log_header, nsec) == 8, ...);
```

字段：

| 字段    | 含义                                           |
| ------- | ---------------------------------------------- |
| `magic` | `0x5a 0xa5`，用于基本完整性检查                |
| `level` | `KERN_*` 级别                                  |
| `flags` | 记录标志                                       |
| `len`   | payload 长度，不含 NUL                         |
| `nsec`  | boot 单调时间，纳秒；timekeeper 未初始化时为 0 |

记录头固定 16 字节、8 字节对齐，没有隐式 padding。

### 2.3 标志

```c
#define PRINTK_FLAGS_CONTINUED (1u << 0) /* 保留：逻辑消息续段 */
#define PRINTK_FLAGS_MORE      (1u << 1) /* 保留：后面还有续段 */
#define PRINTK_FLAGS_WRAP      (1u << 2) /* 环形边界填充记录 */
#define PRINTK_FLAGS_TRUNCATED (1u << 3) /* payload 被截断 */
```

当前 `printk` 使用固定大小的格式化缓冲区，超长消息会被截断并设置 `TRUNCATED`，不会产生逻辑续段。`CONTINUED` / `MORE` 预留给未来的流式格式化实现；reader 会原样把 flags 返回给消费者。

### 2.4 最大长度

```c
#define PRINTK_MAX_PAYLOAD 1024
```

`printk_log_write()` 会把超过上限的 payload 截断到 `PRINTK_MAX_PAYLOAD`，并设置 `PRINTK_FLAGS_TRUNCATED`。

## 3. 环形缓冲区

```c
#define PRINTK_RING_BUFFER_SIZE (1u << 16)
#define PRINTK_RING_BUFFER_MASK (PRINTK_RING_BUFFER_SIZE - 1u)

struct printk_ring_buffer {
	char buf[PRINTK_RING_BUFFER_SIZE]
		__attribute__((aligned(alignof(struct printk_log_header))));
	u64 head;
	u64 tail;
	spinlock_t lock;
};
```

- `head`：单调递增的字节序列，指向下一条记录的写入位置；
- `tail`：单调递增的字节序列，指向最旧仍然完整的记录；
- 实际 ring 偏移为 `seq & PRINTK_RING_BUFFER_MASK`；
- 使用 `u64` 避免 4GiB 绕回比较问题；
- `head - tail` 是当前占用字节数，包含记录头和 padding。

不变量：

```text
tail <= head
head - tail <= PRINTK_RING_BUFFER_SIZE
tail 指向一条完整记录的起点（或尾部小 padding 的起点）
head 指向下一条记录的起点
所有记录起点按 alignof(struct printk_log_header) 对齐
```

### 3.1 不拆分记录

一条记录**不允许跨环形边界**。如果当前尾部剩余空间不足：

- 如果剩余空间足够放下一个记录头，写入一条 `PRINTK_FLAGS_WRAP` 填充记录；
- 如果剩余空间连记录头都放不下，直接零填充并推进 `head` 到环形边界。

reader 遇到：

- `WRAP` 记录：直接跳过；
- `offset + sizeof(header) > PRINTK_RING_BUFFER_SIZE`：说明是尾部小 padding，直接跳到下一个环形边界。

这样避免了“同一条记录有两个 header”“`len` 指整段还是分段”“CONTINUED 是逻辑续段还是绕回续段”等歧义。

### 3.2 覆盖策略

writer 在写入前保证有足够空间：

```text
while (head - tail + need > PRINTK_RING_BUFFER_SIZE)
        tail += oldest_record_total(tail)
```

`oldest_record_total()` 读取 `tail` 处的 header：

- 正常记录：`printk_record_total(header->len)`；
- `WRAP` 记录：同样按 `printk_record_total(header->len)`；
- 尾部小 padding：`PRINTK_RING_BUFFER_SIZE - (tail & MASK)`；
- header 损坏：直接把 `tail` 推到 `head`，丢弃全部旧数据。

writer 先推进 `tail`，再覆盖旧数据，因此 `tail` 始终指向仍然有效的记录边界。

## 4. writer

### 4.1 写入流程

```text
1. 截断超长 payload，设置 TRUNCATED
2. 计算 total = printk_record_total(len)
3. 如果 total 已经大于 ring 可用空间，说明配置非法，丢弃
4. 加 ring->lock
5. 如果尾部空间不足，计算 pad，并把 pad 计入 need
6. while (head - tail + need > SIZE) 丢弃最旧记录
7. 如果 pad 存在：
       pad >= sizeof(header)：写 WRAP 记录
       pad <  sizeof(header)：零填充
       head += pad
8. 写 header + payload + padding
9. head += total
10. 解锁
```

记录写入顺序很重要：先写 header/payload，最后更新 `head`。`head` 的发布代表记录完整可用。

### 4.2 锁

writer 和 reader 都使用 `ring->lock`：

- writer 用 `spin_lock_irqsave()`；
- reader 用 `spin_lock_irqsave()` 拷贝记录；
- `head` / `tail` / `cursor` 的更新都在锁内；
- 因为使用 spinlock，不需要额外的 acquire/release 内存屏障。

## 5. reader

### 5.1 reader 状态

```c
struct printk_log_reader {
	struct printk_ring_buffer *ring;
	u64 cursor;
	u64 pending_lost;
};
```

- `cursor`：下一条待读取记录的字节序列；
- `pending_lost`：该 reader 错过、但还没有报告的 ring 字节数。

`pending_lost` 统计的是**整个 ring 字节数**，包括记录头和对齐填充，不是纯 payload 字节数。

### 5.2 初始化

```c
enum printk_log_start {
	PRINTK_LOG_START_OLDEST, /* 从 tail 开始，回放当前 ring */
	PRINTK_LOG_START_NEWEST, /* 从 head 开始，只看新日志 */
};

void printk_log_reader_init(struct printk_log_reader *r,
			    enum printk_log_start start);
```

- console 注册：`PRINTK_LOG_START_OLDEST`，回放早期启动日志；
- klogd：`PRINTK_LOG_START_OLDEST`，尽量把 ring 中已有日志写入磁盘；
- 运行期动态加入的、不希望回放的 console 可以选择 `NEWEST`。

### 5.3 读取流程

```text
1. 加 ring->lock
2. if (cursor < tail)
       pending_lost += tail - cursor
       cursor = tail
3. if (cursor == head)
       unlock; return 0
4. 读 cursor 处的记录
5. 如果尾部小 padding：
       cursor += SIZE - (cursor & MASK)
       continue
6. 校验 magic / len / total / cursor + total <= head
       失败：pending_lost += head - cursor; cursor = head; 返回 -EIO
7. 如果是 WRAP：cursor += total; continue
8. 如果 payload 放不进调用方缓冲区：
       cursor += total; 返回 -ENOSPC（记录已消费）
9. 拷贝 payload，NUL 结尾
10. cursor += total
11. unlock
12. 返回 payload 长度
```

返回值：

| 返回值    | 含义                                  |
| --------- | ------------------------------------- |
| `> 0`     | payload 长度，`buf` 已 NUL 结尾       |
| `0`       | 当前没有记录                          |
| `-ENOSPC` | 记录存在但 `buf` 不够大，记录已被消费 |
| `-EIO`    | 检测到损坏，已重新同步到 `head`       |

调用方在每次读取后都必须检查 `reader->pending_lost`，即使读取返回 0：

```c
if (reader->pending_lost) {
	report_lost(reader->pending_lost);
	reader->pending_lost = 0;
}
```

### 5.4 丢失报告

console 和 klogd 在写入下一条正常记录之前，先输出一条丢失提示：

```text
<level>[            ] ** N bytes lost **
```

丢失提示不写入 ring，由消费者即时生成，避免它再次被覆盖。

## 6. console 消费

### 6.1 console 结构

```c
struct console {
	void *client_data;
	struct list_head list;
	int (*put_char)(struct console *console, int c);
	bool active;
	u8 log_level;
	struct printk_log_reader reader;
};
```

每个 console 独立消费，互不影响。慢 console 不会阻塞 `printk`，只会让自己落后，最终通过 `pending_lost` 报告丢失。

日志级别也是 per-console 的：

```c
void console_register(struct console *console, u8 log_level);
int console_set_log_level(struct console *console, u8 level);
```

因此可以让 UART 只看 `KERN_INFO`，而调试 console 保留 `KERN_DEBUG`，不需要共享一个全局过滤等级。

启动 console 通过标志标记：

```c
#define CONSOLE_FLAG_BOOT (1u << 0)
```

klogd 打开持久日志文件后，不直接调用具体 UART 驱动接口，而是调用 console core 的通用策略：

```c
void console_set_boot_consoles_level(u8 level);
```

klogd 调用：

```c
console_set_boot_consoles_level(KERN_EMERG);
```

这样带 `CONSOLE_FLAG_BOOT` 的 console 仍然保持 active，但只输出 `KERN_EMERG`。正常内核日志不再刷到 UART，而 panic 仍然可见；用户 tty 也不受影响。

日志子系统不需要 include `<brk/uart.h>`，也不需要知道 `ns16550a` 的存在；具体驱动只在注册时声明自己是不是启动 console。

### 6.2 启动阶段同步输出

在调度器和 console 线程启动之前，`printk` 需要把日志直接送到早期 console，否则启动失败时看不到输出。

采用简单策略：

- 默认 `printk_console_sync = true`；
- `printk` 写入 ring 后，如果同步模式开启，调用一次 `printk_flush_pending()`；
- 启动 console 消费线程前调用 `printk_console_set_async(true)`，关闭同步刷新；
- `printk_flush_pending()` 使用 trylock，拿不到 console 锁时直接返回，避免递归/死锁。

### 6.3 console 线程

调度器可用后启动一个内核线程：

```text
for (;;) {
        if (!printk_flush_pending())
                ktime_nanosleep(10ms);
}
```

`printk_flush_pending()` 遍历所有 active console，逐个消费 reader。console 输出在 `console_lock` 下完成，但 ring 锁只在拷贝记录时短暂持有。

### 6.4 锁顺序

固定顺序：

```text
console_lock -> ring->lock
```

禁止反向顺序。`printk` 写入 ring 后先释放 `ring->lock`，再调用 `printk_flush_pending()`。

console 驱动不应在 `put_char()` 中调用 `printk`；同步刷新使用 trylock，可以避免最基本的递归死锁，但不能替代这条规则。

## 7. klogd 消费

`klogd` 拥有独立的 `struct printk_log_reader`：

- 启动时从 `PRINTK_LOG_START_OLDEST` 开始；
- 每次最多读 `KLOG_BATCH` 条；
- 如果这一批读完仍有积压，不睡眠，立即继续下一批；只有 ring 为空时才睡眠；
- 根据 `KLOG_LEVEL` 过滤；
- 遇到 `TRUNCATED` 时在行尾追加 `...`；
- `pending_lost` 非 0 时先写入丢失提示；
- 每批写入完成后统一 fsync 一次，避免每条日志都触发磁盘同步；
- 读取和磁盘写入都在 `ring->lock` 之外完成（`printk_log_read` 内部只短暂持有 ring 锁）。

## 8. panic 路径

panic 不应该等待正常 reader，也不应该在锁被其他 CPU 持有时死锁。当前实现：

1. `panic()` 用原子 `panic_cpu` 选定唯一的 panic CPU；
2. 先调用 `console_flush_on_panic()`，尽量把 panic 前积压的 `KERN_EMERG` 记录刷到 console；
3. 如果 ring 锁可以 trylock，把 panic 文本也写进 ring；
4. 通过 `console_write_record_panic()` 直接输出到 active console；
5. console 驱动提供 `put_char_atomic` 回调，panic 路径使用 trylock + 有界轮询，避免无限等待 UART 锁；
6. 其他 CPU 进入 `wfi`，不再并发输出。

```c
int panic_cpu = -1;

local_irq_disable();
if (atomic_cmpxchg(&panic_cpu, -1, this_cpu) != -1)
        halt;

console_flush_on_panic();
write_ring_if_trylock_available();
console_write_record_panic(KERN_EMERG, now, text, len);
```

约束：

- 不等待 klogd/console 线程；
- 不分配内存；
- 不调用普通 console 锁路径；
- UART 的 panic 输出是 best-effort，拿不到驱动锁时允许丢字符；
- 当前没有实现 Linux 那样的 `smp_send_stop()`、panic notifier、kdump/pstore 和 `panic_timeout`，这些是后续可扩展项。

## 9. API

```c
/* printk_log.h */
struct printk_log_reader { ... };

size_t printk_log_write(u8 level, const char *text, size_t len, u64 nsec);

void printk_log_reader_init(struct printk_log_reader *r,
			    enum printk_log_start start);

int printk_log_read(struct printk_log_reader *r,
		    char *buf, size_t size,
		    u8 *level, u64 *nsec, u8 *flags);
```

`printk.h` 保留：

```c
void printk(const char *fmt, ...);
void vprintk(const char *fmt, va_list ap);
void _printk(int level, const char *fmt, ...);
bool printk_flush_pending(void);
void printk_console_set_async(bool async);
void printk_panic_text(const char *text, size_t len);
```

## 10. 测试点

1. 正常写入/读取，空 ring 返回 0；
2. 记录刚好填满 ring；
3. 尾部剩余空间不足一个记录头；
4. 尾部剩余空间足够写 `WRAP` 记录；
5. writer 覆盖慢 reader，`pending_lost` 精确增加；
6. reader 落后超过一整圈；
7. 多个 console 独立消费，快 reader 不被慢 reader 拖累；
8. console 注册时回放已有日志；
9. klogd 启动时回放 ring；
10. `TRUNCATED` 超长消息；
11. 并发 `printk` + reader；
12. panic 时另一个 CPU 持有 ring 锁；
13. 中断上下文 `printk`。

## 11. 当前限制

- 暂不实现逻辑消息分段；超长消息截断并置 `TRUNCATED`；
- `CONTINUED` / `MORE` 为未来流式格式化预留，当前 writer 不产生；
- `magic` 只用于完整性 sanity check，正常读取以 `tail` / `cursor` 为可信位置；
- console 驱动仍应避免在输出回调中调用 `printk`；
- boot console 被 klogd 降到 `KERN_EMERG`，仍保留 panic 输出能力；
- panic 路径尚未实现 `smp_send_stop()`、panic notifier、kdump/pstore 和 `panic_timeout`。
