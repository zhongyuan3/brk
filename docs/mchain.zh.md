# mchain 物理内存分配器设计文档

## 1. 概述

**mchain**（memory chain）是一个面向内核早期启动阶段的物理内存分配器。它借鉴 Linux memblock 的“可用内存 + 预留区域”语义模型，但底层采用**数组式有序双向链表**实现：所有区域条目存放在固定大小的静态数组中，每个条目通过有符号下标 `next` / `prev` 指向相邻区域，链表按物理地址升序排列。条目携带状态位与属性位，仅当状态、属性、NUMA 节点均相同且地址连续时才允许合并；`ALLOCATED` 条目永不参与合并，以保留每次分配的精确边界。

**设计目标**：

- 不依赖页分配器、slab 或任何动态内存机制即可初始化并工作。
- 支持不连续物理内存区域、预留、分配、释放、对齐与地址范围限制。
- 提供可供页分配器初始化消费的区间迭代接口。
- 数据结构可审计、可 dump，便于启动期 bring-up 调试。

**非目标**：运行期高性能分配（启动后应移交给伙伴系统）、内存热插拔的完整支持、复杂 NUMA 距离策略。

---

## 2. 术语

| 术语           | 含义                                                                     |
| -------------- | ------------------------------------------------------------------------ |
| 区域（region） | 一段连续的物理地址区间 `[base, base+size)`                               |
| 条目（entry）  | 数组中的一个元素，描述一个区域并持有链表指针                             |
| arena          | 存放条目的数组空间，初始为静态数组，可扩容                               |
| 状态位         | 区域当前处于 FREE / RESERVED / ALLOCATED 之一                            |
| 属性位         | 区域的内存属性，如 HOTPLUG / NOMAP / MIRROR / DRIVER                     |
| 合并           | 两个相邻条目在状态、属性、nid 均相同时合并为一个条目（`ALLOCATED` 除外） |

---

## 3. 数据结构

```c
/* 状态位（互斥，低 4 位） */
#define MCHAIN_STATE_FREE        0x0u
#define MCHAIN_STATE_RESERVED    0x1u
#define MCHAIN_STATE_ALLOCATED   0x2u
#define MCHAIN_STATE_MASK        0xfu

/* 属性位（可组合，高位） */
#define MCHAIN_ATTR_HOTPLUG      (1u << 4)
#define MCHAIN_ATTR_NOMAP        (1u << 5)
#define MCHAIN_ATTR_MIRROR       (1u << 6)
#define MCHAIN_ATTR_DRIVER       (1u << 7)
#define MCHAIN_ATTR_MASK         0xfffffff0u

#define MCHAIN_NID_ANY           (-1)
#define MCHAIN_ANY_ADDR          ((phys_addr_t)~(phys_addr_t)0)

#ifndef MCHAIN_STATIC_CAPACITY
#define MCHAIN_STATIC_CAPACITY   256
#endif

#define MCHAIN_NAME_SLOTS        32
#define MCHAIN_NAME_MAX          32
#define MCHAIN_NAME_NONE         0u

struct mchain_entry {
    phys_addr_t  base;      /* 区域起始物理地址 */
    phys_size_t  size;      /* 区域大小，字节 */
    int          next;      /* 下一区域在 arena 中的下标，-1 表示结尾 */
    int          prev;      /* 上一区域在 arena 中的下标，-1 表示开头 */
    int          nid;       /* NUMA 节点号，MCHAIN_NID_ANY 表示无关 */
    unsigned int flags;     /* 状态位 | 属性位 */
    unsigned int name_id;   /* 命名预留 id，MCHAIN_NAME_NONE 表示无名 */
};

struct mchain_pool {
    struct mchain_entry *entries;   /* 当前 arena 基址 */
    unsigned int capacity;  /* arena 条目容量 */
    unsigned int used;      /* 已占用条目数 */
    int          head;      /* 有序区域链表头下标，-1 表示空 */
    int          free_head; /* 空闲条目链表头，-1 表示尚未建立 */
    bool         bottom_up; /* true: bottom-up；false: top-down */
    int          can_grow;  /* 是否允许扩容 */
    int          frozen;    /* 是否已冻结，冻结后禁止任何区域修改 */
    int          free_list_ready; /* 静态空闲链表是否已建立 */
    phys_addr_t  min_limit; /* 全局分配下限，默认 0 */
    phys_addr_t  max_limit; /* 全局分配上限，默认 MCHAIN_ANY_ADDR */
    /* 调试统计，见 §10 */
    unsigned int peak_used;       /* 峰值 used */
    unsigned int grow_count;      /* arena 扩容次数 */
    unsigned int alloc_failures;  /* mchain_alloc() 失败次数 */
};

/* 静态 arena 与全局池：定义时直接初始化，无需显式 init */
static struct mchain_entry mchain_static_entries[MCHAIN_STATIC_CAPACITY];

struct mchain_pool mchain_pool_instance = {
    .entries         = mchain_static_entries,
    .capacity        = MCHAIN_STATIC_CAPACITY,
    .used            = 0,
    .head            = -1,
    .free_head       = -1,             /* 首次修改时由 mchain_bootstrap() 建立 */
    .bottom_up       = true,
    .can_grow        = 0,
    .frozen          = 0,
    .free_list_ready = 0,
    .min_limit       = 0,
    .max_limit       = MCHAIN_ANY_ADDR,
    .peak_used       = 0,
    .grow_count      = 0,
    .alloc_failures  = 0,
};
```

以上代码假设 `phys_addr_t` / `phys_size_t` 和 `<stdbool.h>` 已由内核头文件提供。

**设计要点**：

- 取消 `typedef struct { ... } mchain_*;` 形式，所有类型都带 `struct` 关键字并使用完整的 `mchain_` 前缀：`struct mchain_entry`、`struct mchain_pool`、`struct mchain_iter`、`struct mchain_stats`。
- 全局池在定义时直接初始化，和 memblock 一样不需要显式的 `mchain_init()`。静态字段（限制、方向、统计）由初始化器设置；空闲条目链表不能在 C 静态初始化器中方便地列出，因此由内部函数 `mchain_bootstrap(&mchain_pool_instance)` 在第一次需要时惰性建立，并把 `free_list_ready` 置 1。
- `next`、`prev` 与 `free_head` 均使用 `int`，`-1` 作为终止哨兵，避免额外布尔字段。
- 区域链表为双向链表，`prev` 使 top-down 分配可以直接从尾部反向扫描，不需要临时候选数组。所有插入、摘除和分裂操作必须同时维护 `next` 与 `prev`。
- `entries` 是指针而非数组，使静态 arena 与扩容 arena 共用同一套操作代码。
- 空闲条目通过独立的 `free_head` 单链表管理，与区域链表完全解耦。条目被回收时压入空闲链表，分配时从头部弹出。
- `bottom_up` 是全局分配方向，默认 `true`；通过 `mchain_set_bottom_up()` 修改，不再给 `mchain_alloc()` 传 `direction` 参数。
- `can_grow` 在扩容过程中置 0，防止扩容路径递归触发扩容。初始值为 0，只有直接映射可用后才能通过 `mchain_allow_grow()` 打开。
- `frozen` 在页分配器交接前由 `mchain_freeze()` 置 1，之后所有区域修改 API 和 `mchain_alloc()` 都拒绝服务，防止已经交给伙伴系统的内存被 mchain 再次分配。
- 所有公开 API 操作全局 `mchain_pool_instance`；需要多实例的内部辅助函数接收 `struct mchain_pool *` 参数，便于单元测试。

---

## 4. 链表维护

### 4.1 查找插入位置

按 `base` 升序遍历双向链表，维护插入点前驱下标 `prv` 与当前下标 `cur`。插入位置满足：`prv == -1 || entries[prv].base + entries[prv].size <= new->base` 且 `cur == -1 || new->base + new->size <= entries[cur].base`。重叠区域的插入属于调用者错误，在调试构建下触发断言。生产构建中内部插入函数仍应返回 `-EINVAL`，由上层保证不会发生。

### 4.2 合并规则

两个条目可合并当且仅当：

```
state 相同 && state != ALLOCATED && attr 相同 && nid 相同 && 地址连续
```

若启用命名预留（§9），还要求 `name_id` 相同。地址连续指 `a.base + a.size == b.base`。合并后 `base` 取较小者，`size` 相加，被吸收条目的 `next` / `prev` 被跳过并归还空闲链表。

**不合并的情况**：

- `ALLOCATED` 条目既不与 `FREE` / `RESERVED` 合并，也不与相邻的 `ALLOCATED` 合并。这是硬性规则：每次分配必须保留独立条目，否则两次相邻分配会被合并成一块，`mchain_free()` 无法用原始 `(base, size)` 定位并释放其中一次分配。
- 跨状态不合并：`RESERVED` 与 `FREE` 即使地址连续也保持独立，预留区域始终可见。
- 属性或 `nid` 不同的相邻区域不合并，因为映射语义或 NUMA 归属不同。
- 命名预留的 `name_id` 不同则不合并，避免按名字查找时把不同预留混在一起。

`RESERVED` 与 `RESERVED` 在键相同时会合并，因此预留区可能变成一个较大的条目；释放预留必须使用支持部分范围的 `mchain_unreserve()`，不能依赖条目边界。

### 4.3 插入（`mchain_insert_entry`）

1. 调用者必须先通过 `mchain_ensure_entries()` 预分配所需条目（见 §4.5 和 §7）。`mchain_insert_entry()` 自身不触发扩容，进入插入阶段后不会因条目不足而失败。
2. 填充 `base`、`size`、`state | attrs`、`nid`；若启用命名预留，还要填充 `name_id`（普通区域为 `MCHAIN_NAME_NONE`）。
3. 定位插入位置，检查与前驱、后继的可合并性；`ALLOCATED` 条目跳过合并检查，直接插入：
   - 仅前驱可合并：扩展前驱 `size`；若前驱的新尾部又与后继可合并，吸收后继。新条目本身没有被使用，必须归还空闲链表。
   - 仅后继可合并：新条目扩展覆盖后继，回收后继条目，新条目接替其后继指针并修正 `prev`。
   - 前后均可合并：扩展前驱覆盖一切，回收后继条目；新条目本身没有被使用，必须归还空闲链表。
   - 均不可合并：新条目插入 `prv` 与 `cur` 之间，并修正两侧的 `prev` / `next`。
4. 归还被吸收条目或未被使用的新条目之前，先把它们从 `next` / `prev` 链上完整摘下，再压入空闲链表。

### 4.4 切除（`mchain_carve_range`）

从区域链表中挖去 `[start, end)` 中满足谓词 `(state, attr_mask, nid)` 的字节，用于预留、分配和取消预留。**不是无条件挖去整个范围**：不匹配的条目必须原样保留，否则预留或分配会误删其他状态的内存。

遍历链表，对每个匹配且相交的条目分类处理：

- **完全覆盖**（`start <= base && base + size <= end`）：从链表摘除，条目归还空闲链表。
- **覆盖左半**（`start <= base < end < base+size`）：调整 `base = end`，`size = old_base + old_size - end`。注意不能写成 `size -= (end - start)`，因为 `start` 可能小于 `base`。
- **覆盖右半**（`base < start < base+size <= end`）：`size = start - base`。
- **覆盖中间**（`base < start && end < base+size`）：原条目保留左片段（`size = start - base`），新建条目承载右片段（`base = end`，`size = old_end - end`，状态/属性/nid/name_id 复制），新条目的 `next` 指向原条目的旧 `next`，原条目的旧后继的 `prev` 指向新条目，再令原条目 `next` 指向新条目、新条目 `prev` 指向原条目。**顺序不可颠倒**，否则丢失后继链。

切除只修改链表结构和条目归属，不改变剩余片段的标志位；切除出的区间由调用者决定以何种状态重新插入。中间分裂需要额外条目，调用者必须提前预分配；否则 `mchain_carve_range()` 返回 `-ENOMEM`，且不得留下部分修改。

### 4.5 修改的原子性

所有需要新增条目的公开修改 API 遵循“计数 → 预分配 → 修改”的顺序：

1. 只读扫描，计算本次操作需要的新条目数。对一次分裂，最多需要 2 个额外条目（左片段、右片段、被切除片段三者中可复用原条目承载其中一个）；如果候选正好覆盖整个空闲条目，则原条目可以直接原地改变状态，不需要额外条目；覆盖条目一端时只需要 1 个。实现应按实际位置精确计算，而不是一律按 2 个预分配，否则 arena 只剩少量槽位时可能不必要地触发扩容甚至失败。注意不要先把整个条目切除再无条件插入，否则“覆盖整段”也会错误地多要一个条目。
2. 调用 `mchain_ensure_entries(pool, nr, avoid_start, avoid_end)`。若空闲条目不足且允许扩容，则触发 `mchain_grow_pool()`；若仍不足，返回 `-ENOMEM`，此时目标 `avoid` 范围未被修改（但可能已经完成了部分扩容并消耗了其他空闲内存）。`avoid` 区间表示当前操作要保留的目标范围，扩容不得从该区间中分配新 arena，否则预留、取消预留或分配的语义会被扩容自身改变。
3. 执行切除和插入。由于条目已经预分配，这一阶段不允许因条目不足而失败。

扩容本身会消耗一段空闲内存作为新 arena，并把这段区间登记为 `ALLOCATED`；这是扩容的预期副作用，不属于目标操作的原子范围。因此“扩容成功但随后分配失败”会留下更大的 arena 和它占用的内存，但不会留下目标分配、预留或取消预留的部分修改。`mchain_ensure_entries()` 必须保证扩容过程中不会递归调用需要预分配的公开 API。

---

## 5. 公共 API 概览

完整的函数签名、参数约束、返回值和错误码见 §14；本节只说明核心语义。所有 API 在失败时返回错误码或特殊值，不触发 panic（除调试断言外）。

### 5.1 静态初始化、分配方向与生命周期

```c
int  mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
void mchain_set_bottom_up(bool bottom_up);
bool mchain_bottom_up(void);
void mchain_allow_grow(void);
void mchain_freeze(void);
```

全局池 `mchain_pool_instance` 在定义时直接静态初始化：空链表、限制 `[0, MCHAIN_ANY_ADDR)`、`bottom_up = true`、`can_grow = 0`、`frozen = 0`。和 memblock 一样，**不需要显式的 `mchain_init()`**。空闲条目链表无法用普通 C 静态初始化器方便地列出，因此由内部函数 `mchain_bootstrap()` 在第一次需要空闲链表的修改 API 中惰性建立；在此之前查询和迭代器看到的是空链表。

`mchain_set_limit()` 可在启动过程中调整全局分配上下限，例如直接映射范围扩大后。它只影响后续分配，不修改已有区域；`max_addr == MCHAIN_ANY_ADDR` 表示不设上限。参数非法时返回 `-EINVAL`，原有设置保持不变。

`mchain_set_bottom_up()` 设置全局分配方向：`true` 为 bottom-up，`false` 为 top-down，默认 `true`。它参考 memblock 的 `memblock_set_bottom_up()`，只影响后续 `mchain_find_in_range()` / `mchain_alloc()`，不改变已有区域。`mchain_bottom_up()` 返回当前方向。

`mchain_allow_grow()` 在直接映射建立后调用，允许 arena 按需扩容。在此之前静态 arena 必须足够大；扩容路径依赖 `phys_to_virt()` 访问新 arena。

`mchain_freeze()` 在把空闲内存交接给伙伴系统之前调用。冻结后 `mchain_alloc()` 和用于规划分配的 `mchain_find_in_range()` 返回 0，`mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` / `mchain_free()` 返回 `-EBUSY`，扩容被关闭；尺寸/状态查询、dump 和迭代器仍然可用，便于完成交接。冻结是不可逆的（除非重新初始化全局池，仅测试场景）。

### 5.2 添加、预留与取消预留

```c
int mchain_add(phys_addr_t base, phys_size_t size,
               unsigned int attrs, int nid);
int mchain_reserve(phys_addr_t base, phys_size_t size,
                   unsigned int attrs, int nid);
int mchain_unreserve(phys_addr_t base, phys_size_t size);
```

`mchain_add()` 只添加 `FREE` 区域。目标范围与已有条目重叠时，已有条目覆盖的字节**保持不变**，只有未被覆盖的字节以 `FREE | attrs` 插入；新插入的间隙可以与相邻的同键 `FREE` 条目合并。这与 memblock 的 `memblock_add()` 语义一致。不要用 `mchain_add()` 添加预留区或分配区。

`mchain_reserve()` 只作用于目标范围内的 `FREE` 字节：将这些字节切除后以 `MCHAIN_STATE_RESERVED | (源属性 | attrs)` 插入。保留源空闲区的属性是为了避免把 NOMAP / HOTPLUG / MIRROR / DRIVER 等物理内存属性因为状态改变而丢掉；`attrs` 只能追加属性，不能清除源属性。目标范围中已经属于 `RESERVED` 或 `ALLOCATED` 的字节保持不变，因此部分重叠是允许的，且不会覆盖已有分配。需要严格确认整个范围是否可预留时，先调用 `mchain_range_is_free()`。只有已经由 `mchain_add()` 登记为 `FREE` 的字节才能被预留；如果目标范围尚未添加（位于空洞），该部分不会被记录为预留，因此启动流程必须**先添加完所有可用内存，再执行预留**。

`mchain_unreserve()` 是 `mchain_reserve()` 的逆操作：只把目标范围内的 `RESERVED` 字节转回 `FREE`，支持部分释放；`FREE` 和 `ALLOCATED` 字节不受影响。

### 5.3 查找、分配与释放

```c
phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
                                 phys_addr_t min_addr, phys_addr_t max_addr,
                                 int nid, unsigned int attr_mask);
phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
                         phys_addr_t min_addr, phys_addr_t max_addr,
                         int nid, unsigned int attr_mask);
int mchain_free(phys_addr_t base, phys_size_t size);
```

`mchain_find_in_range()` 只查找、不修改链表；`mchain_alloc()` 在相同查找规则上切除并插入一个 `ALLOCATED` 条目。

- `min_addr` / `max_addr` 为 `MCHAIN_ANY_ADDR` 时分别替换为全局 `min_limit` / `max_limit`；替换后再与全局限制求交。`max_addr` 是开区间上界。
- `nid == MCHAIN_NID_ANY` 时任意节点可满足；指定节点时，条目的 `nid` 等于该节点或为 `MCHAIN_NID_ANY` 都匹配。
- `attr_mask` 是必须匹配的属性位，只使用 `MCHAIN_ATTR_MASK` 中的位。`MCHAIN_ATTR_NOMAP` 和 `MCHAIN_ATTR_DRIVER` 默认被跳过，只有显式在 `attr_mask` 中要求时才参与分配，避免普通分配误拿不可直接映射或驱动管理的内存。
- 分配方向由全局 `mchain_set_bottom_up()` 决定，默认 bottom-up；不再通过参数传入。
- 候选区间必须完整落在代入 `MCHAIN_ANY_ADDR` 规则后的有效范围内，并满足对齐；`align` 必须是 2 的幂。
- 成功时只分配精确的 `[candidate, candidate + size)`，**不吞并对齐产生的碎片**，因此 `mchain_free(base, size)` 可以用调用者保存的原始大小精确匹配。分配条目继承源空闲条目的全部属性与 `nid`。
- 失败返回 `0`；物理地址 `0` 被保留为失败哨兵，不会被分配。

`mchain_free()` 按 `(base, size)` 查找完全匹配的 `ALLOCATED` 条目，将其转为 `FREE` 并与相邻空闲条目合并。部分释放不支持；释放预留区应使用 `mchain_unreserve()`。

### 5.4 查询、迭代与调试

```c
bool mchain_range_is_free(phys_addr_t base, phys_size_t size);
bool mchain_is_memory(phys_addr_t addr);
phys_size_t mchain_free_size(void);
phys_size_t mchain_reserved_size(void);
phys_size_t mchain_memory_size(void);

void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr, phys_addr_t max_addr,
                      int nid, unsigned int attr_mask);
int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);

int mchain_validate(void);
void mchain_dump(void);
```

查询函数只读，不触发扩容。迭代器只返回 `FREE` 区间，跳过不匹配的 `nid` / 属性以及与地址限制无交集的条目；迭代期间不得修改链表。命名预留的 `mchain_reserve_named()` / `mchain_find_reserved()` 是可选项，见 §9。

---

## 6. 分配路径细节

### 6.1 对齐与碎片处理

首版**不把对齐产生的碎片并入分配区**。`mchain_alloc()` 始终只分配精确的 `[candidate, candidate + size)`，这样 `mchain_free(base, size)` 可以直接用调用者保存的原始大小匹配 `ALLOCATED` 条目。若把左侧或右侧的小碎片并进分配条目，分配条目的实际范围会大于请求值，而调用者并不知道实际大小，释放时必然失配。

因此 `MCHAIN_MIN_FRAG` 在首版不启用，仅作为未来优化预留。若将来确实要合并碎片，必须同时提供实际分配大小的输出（例如增加 `mchain_alloc_ex(..., phys_size_t *allocated_size)`），并由调用者把实际大小传给 `mchain_free()`；不能只改分配路径而不改释放契约。

### 6.2 地址限制的检查顺序

先按 `MCHAIN_ANY_ADDR` 规则把 `min_addr` / `max_addr` 替换为全局限制，再计算有效范围 `[lo, hi) = [max(min_addr, pool.min_limit), min(max_addr, pool.max_limit))`，然后按 `lo` / `hi` 过滤条目（条目区间与有效范围无交集则跳过），最后在条目内计算候选起点并做二次检查。这样避免在不可能满足的条目上做对齐计算，也保证全局限制不会被参数绕过。

### 6.3 反向遍历

首版采用**双向链表**：`struct mchain_entry` 已包含 `prev` 下标。当全局方向为 top-down（`mchain_bottom_up() == false`）时，分配从链表尾开始沿 `prev` 反向扫描，时间复杂度与 bottom-up 相同。所有插入、摘除、合并和中间分裂操作都必须同时维护 `next` 与 `prev`，二者对称使用 `-1` 哨兵。

不采用“正向遍历收集候选下标到临时数组”的方案：该方案需要额外的容量上限、回退逻辑，并且在扩容或链表变化时更容易出错。双向链表多出的一个 `int` 字段远低于临时数组的复杂度成本。

---

## 7. 动态扩容

### 7.1 触发条件与前置条件

所有可能新增条目的操作在修改链表前调用 `mchain_ensure_entries(pool, nr, avoid_start, avoid_end)` 预分配条目；当空闲条目不足且 `can_grow == 1` 时，由 `mchain_ensure_entries()` 触发 `mchain_grow_pool()`。`mchain_insert_entry()` 自身不触发扩容，因此一旦进入修改阶段就不会因为缺少条目而失败。

`avoid` 区间是当前操作必须保留的目标范围。扩容需要从空闲内存中分配新 arena，如果这段内存恰好落在当前要预留、取消预留或分配的范围内，操作语义就会被扩容自身改变。因此扩容必须在 `[0, avoid_start)` 和 `[avoid_end, max_limit)` 两段中寻找空间，不能与 `avoid` 相交；`avoid_end == avoid_start` 表示不限制。

`can_grow` 初始为 0。只有直接映射已经建立、`phys_to_virt()` 可以访问新 arena 时，调用 `mchain_allow_grow()` 才把它置 1。启动关键路径上应预留足够大的 `MCHAIN_STATIC_CAPACITY`，使扩容只作为兜底。

### 7.2 扩容流程（自举）

扩容有一个关键的自举约束：触发扩容时旧 arena 已经没有任何空闲条目，因此**不能**通过“先用 `mchain_alloc()` 插入一个 ALLOCATED 条目来登记新 arena”的方式完成扩容——那条路径本身就需要空闲条目。正确的顺序如下：

1. 置 `can_grow = 0`，计算 `new_capacity = old_capacity * MCHAIN_GROW_FACTOR + MCHAIN_GROW_HEADROOM`（默认 `2 * old + 2`），并检查乘法、加法溢出。
2. 计算 `new_bytes = align_up(new_capacity * sizeof(struct mchain_entry), MCHAIN_ARENA_ALIGN)`，检查乘法、加法和 `align_up` 不回绕；然后用只读的 `mchain_find_in_range()`（或等价的内部查找函数）找到 `new_bytes` 字节的空闲物理区间。查找必须避开当前操作的 `avoid` 区间：先尝试 `[0, avoid_start)`，失败后再尝试 `[avoid_end, max_limit)`；`avoid_end == avoid_start` 时只做一次全范围查找。arena 必须在内核生命周期内保持稳定，因此应跳过 `MCHAIN_ATTR_HOTPLUG` 和 `MCHAIN_ATTR_DRIVER` 区域。此步不修改链表。
3. 通过 `phys_to_virt()` 取得新 arena 的虚拟地址，按区域链顺序把旧 arena 中的有效条目**紧凑复制**到新 arena，重建每个条目的 `next` / `prev`；剩余槽位重建 `free_head`。紧凑复制按链顺序重新编号，不需要旧下标到新下标的映射表。
4. 切换 `entries`、`capacity` 到新 arena。此时新 arena 已经有空闲槽位。
5. 用新 arena 中的槽位把第 2 步找到的物理区间登记为 `ALLOCATED`（必要时分裂原 `FREE` 条目），并继承源空闲条目的 `attrs` / `nid`。`MCHAIN_GROW_HEADROOM` 保证这一步以及调用者后续的插入都有足够槽位。
6. 若旧 arena 是动态分配的（`old_entries != mchain_static_entries`），通过 `mchain_free()` 归还旧 arena。**静态 arena 位于内核镜像/BSS，不能归还**，否则会把内核数据当成空闲内存。
7. 置 `can_grow = 1`，`grow_count++`，返回 0。

第 5 步登记 arena 自身时不能再调用公开的 `mchain_alloc()`，否则会递归进入扩容；应使用内部的“切除空闲条目 + 插入 ALLOCATED”路径，并由 `can_grow == 0` 保证不会重入。

### 7.3 失败处理

- 第 2 步找不到足够大的空闲区间：恢复 `can_grow = 1`，返回 `-ENOMEM`，旧链表不变。
- 第 3 步之后的失败属于实现错误（例如 `MCHAIN_GROW_HEADROOM` 不足）；调试构建应断言。生产构建中应通过容量余量保证这些步骤不会失败。
- 调用者（`mchain_add()`、`mchain_reserve()`、`mchain_alloc()` 等）在预分配失败时直接返回 `-ENOMEM` 或 `0`，不修改链表。
- 扩容成功后即使后续目标操作失败，目标区域不会留下部分修改；但扩容已经消耗了一段空闲内存作为 arena，并把它登记为 `ALLOCATED`，这部分内存不会再回到空闲状态，除非将来显式释放 arena。

---

## 8. 迭代器接口

供页分配器初始化消费空闲区间，避免调用者理解链表内部结构。

```c
struct mchain_iter {
    int          cur;        /* 当前条目下标，仅由迭代器内部维护 */
    phys_addr_t  min_addr;
    phys_addr_t  max_addr;
    int          nid;
    unsigned int attr_mask;
};

void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
                      phys_addr_t max_addr, int nid, unsigned int attr_mask);

int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);
```

`mchain_iter_next()` 跳过非 `FREE` 条目、不匹配 `nid` / 属性的条目、以及与地址限制无交集的条目；对匹配条目返回其与 `[min_addr, max_addr)` 的交集子区间，成功返回 1，遍历结束返回 0。每个条目最多产生一个区间，因此不需要 `offset` 字段。

- `min_addr` / `max_addr` 为 `MCHAIN_ANY_ADDR` 时分别表示不设下限 / 上限；`max_addr` 是开区间上界。
- 与分配路径一致，`MCHAIN_ATTR_NOMAP` 和 `MCHAIN_ATTR_DRIVER` 默认被跳过，只有显式在 `attr_mask` 中要求时才返回这类区间。
- 迭代器只读，不触发扩容；迭代期间修改链表会使 `cur` 失效，调用者必须先完成修改再重新初始化迭代器。
- 这个接口直接对应 memblock 的 `for_each_free_mem_range()`，是页分配器初始化和 `mchain_free_all()` 类交接函数的基础。交接时应先调用 `mchain_freeze()`，再遍历空闲区间把它们交给伙伴系统，防止遍历过程中或之后又有 mchain 分配从这些区间取走内存。

---

## 9. 命名预留

若 kexec 或类似场景需要按名字查找预留区域，在 `struct mchain_entry` 上无法直接存字符串（条目定长）。当前实现采用以下方案：

- 增加 `unsigned int name_id`，指向一张静态字符串表；`MCHAIN_NAME_NONE`（0）表示无名。`mchain_reserve_named()` 接收 `name`，在表中查找或登记后写入 `name_id`。
- 查找接口遍历链表匹配 `name_id`，返回第一个匹配的预留区间。

`name_id` **必须参与合并条件**，否则相邻的“有名预留”和“无名预留”会被合并，名字随条目一起丢失；两个不同名字的相邻预留也会被错误合并。合并键变为：

```
state 相同 && state != ALLOCATED && attr 相同 && nid 相同 &&
name_id 相同 && 地址连续
```

这样只有同名、同属性、同节点的相邻预留才合并。名字只属于 `RESERVED` 状态：`FREE` 和 `ALLOCATED` 条目的 `name_id` 必须是 `MCHAIN_NAME_NONE`，`mchain_unreserve()` 释放出的空闲片段不能继承名字，否则会阻碍空闲内存合并。若同一名字对应多个不连续区间，`mchain_find_reserved()` 返回第一个匹配项；需要枚举全部时，可扩展迭代器使其接受状态过滤，或直接遍历区域链表。

本实现已将 `name_id` 放入 `struct mchain_entry`，并实现 `mchain_reserve_named()` / `mchain_find_reserved()`。若不需要命名预留，所有普通区域的 `name_id` 保持 `MCHAIN_NAME_NONE`，不会影响其他路径。

---

## 10. 调试与统计

- **`mchain_dump()`**：按链表顺序打印 `[base, base+size)`、状态名、属性名、`nid`、`name_id`、条目下标以及 `prev` / `next` 指针，便于发现链表断裂或错误合并。扩容后还应打印 arena 基址、容量、`used` 和 `free_head`。
- **`mchain_validate()`**：先检查未 bootstrap 时的前置条件（`head` / `free_head` 为 -1、`used` 为 0），再确保 `mchain_bootstrap()` 已执行，然后检查 §14.12 列出的全部不变量，正常返回 0，失败返回 `-EINVAL`。可在每次修改后用 `assert(mchain_validate() == 0)` 调用（仅调试构建）。
- **`mchain_get_stats()`**：返回 `used` / `peak_used`、容量、扩容次数、分配失败次数、冻结标志以及 `FREE` / `RESERVED` / `ALLOCATED` 字节数，启动后期输出一次汇总，用于调参 `MCHAIN_STATIC_CAPACITY`。
- **断言**：在 `mchain_insert_entry` 入口检查重叠；在切除分裂后检查链表有序性和 `prev` / `next` 对称性；在扩容迁移后检查下标合法性。调试构建启用，生产构建编译掉。

---

## 11. 配置常量与选型建议

| 常量                     | 建议值      | 说明                                                        |
| ------------------------ | ----------- | ----------------------------------------------------------- |
| `MCHAIN_STATIC_CAPACITY` | 256 ~ 512   | 按最坏情况估算：固件映射条目 × 2 + 内核预留 × 2 + 分裂余量  |
| `MCHAIN_GROW_FACTOR`     | 2           | 每次扩容容量翻倍                                            |
| `MCHAIN_GROW_HEADROOM`   | 2           | 扩容后额外槽位，供登记新 arena 自身和后续分裂使用           |
| `MCHAIN_ARENA_ALIGN`     | `PAGE_SIZE` | 新 arena 的物理对齐；也便于 `phys_to_virt()` 访问           |
| `MCHAIN_MIN_FRAG`        | 0（首版）   | 首版不合并碎片；启用时必须同时提供实际分配大小输出，见 §6.1 |

---

## 12. 实施顺序

1. **核心双向链表与合并/切除**：静态初始化的 `mchain_pool_instance`、`mchain_bootstrap`、`mchain_insert_entry`、`mchain_carve_range`，配合单元测试覆盖相邻合并、`ALLOCATED` 不合并、重叠处理、中间分裂、属性不同不合并、`prev` / `next` 对称性。
2. **预分配与原子性**：`mchain_ensure_entries`、静态 arena 的 free list、修改前计数与预分配，测试“目标区域失败不留下部分修改”（允许扩容已经完成并消耗内存）。
3. **添加、预留、取消预留**：`mchain_add`、`mchain_reserve`、`mchain_unreserve`，覆盖部分重叠和部分释放。
4. **分配与释放**：`mchain_find_in_range`、`mchain_alloc`（先只支持 bottom-up、无地址限制）、`mchain_free`，确认精确分配大小和 `ALLOCATED` 独立条目。
5. **迭代器**：`mchain_iter_*`，并用它验证“空闲区间总和 = 可用内存 − 预留 − 已分配”。
6. **地址限制与 top-down**：使用已有的 `prev` 下标实现反向扫描，通过 `mchain_set_bottom_up(false)` 选择 top-down，并加入 `min_addr` / `max_addr` 过滤和 `MCHAIN_ANY_ADDR`。
7. **flags 拆分与 nid**：属性位与节点号进入合并条件和匹配规则，回归测试全部合并场景。
8. **动态扩容**：按 §7.2 的自举顺序实现，重点测试静态 arena 不归还、动态 arena 归还、`MCHAIN_GROW_HEADROOM` 足够。
9. **调试与统计**：`mchain_validate`、`mchain_dump`、`mchain_get_stats` 可穿插在任意阶段，越早越好。
10. **交接与冻结**：实现 `mchain_freeze()`，在页分配器交接前冻结分配器，再用迭代器把所有 `FREE` 区间交给伙伴系统。

---

## 13. 与 memblock 的语义对照

| memblock                     | mchain                                     |
| ---------------------------- | ------------------------------------------ |
| `memblock_add`               | `mchain_add(..., attrs, nid)`（只加 FREE） |
| `memblock_reserve`           | `mchain_reserve`                           |
| `memblock_remove`            | 无直接对应；用 `mchain_unreserve` 释放预留 |
| `memblock_phys_free`         | `mchain_free` / `mchain_unreserve`         |
| `memblock_find_in_range`     | `mchain_find_in_range`                     |
| `memblock_phys_alloc`        | `mchain_alloc`                             |
| `memblock_set_current_limit` | `mchain_set_limit` + 每次调用参数覆盖      |
| `memblock_bottom_up`         | `mchain_bottom_up`                         |
| `memblock_set_bottom_up`     | `mchain_set_bottom_up`                     |
| `for_each_free_mem_range`    | `mchain_iter_next`                         |
| `memblock_double_array`      | `mchain_grow_pool`                         |
| `memblock_free_all`          | `mchain_freeze` + `mchain_iter_next` 交接  |
| `memblock_dump_all`          | `mchain_dump`                              |

memblock 用 `memory` 和 `reserved` 两个数组表达“可用 − 预留”；mchain 用单一链表上的 `FREE` / `RESERVED` / `ALLOCATED` 状态表达同一语义。因此 `mchain_add()` 只负责添加可用内存，预留和分配必须走各自的状态转换 API，不能把预留/分配状态通过 `mchain_add()` 塞进链表。这套对照让熟悉 memblock 的开发者能快速上手，也便于在 bring-up 阶段交叉验证两者的输出是否一致。

---

## 14. 完整 API 参考

> 本章给出 mchain 的最终 API 契约。§5 是语义概览；若本章与前面章节的签名或细节冲突，以本章为准，并在实现时同步修订前文。所有符号使用完整的 `mchain_` / `MCHAIN_` 前缀；类型一律保留 `struct` 关键字，不使用 typedef。

### 14.1 公共常量与类型

```c
/* 容量与扩容 */
#ifndef MCHAIN_STATIC_CAPACITY
#define MCHAIN_STATIC_CAPACITY       256
#endif
#define MCHAIN_GROW_FACTOR           2
#define MCHAIN_GROW_HEADROOM         2
#define MCHAIN_ARENA_ALIGN           PAGE_SIZE

/* 状态位：互斥，位于低 4 位 */
#define MCHAIN_STATE_FREE            0x0u
#define MCHAIN_STATE_RESERVED        0x1u
#define MCHAIN_STATE_ALLOCATED       0x2u
#define MCHAIN_STATE_MASK            0xfu

/* 属性位：可组合，位于高位 */
#define MCHAIN_ATTR_HOTPLUG          (1u << 4)
#define MCHAIN_ATTR_NOMAP            (1u << 5)
#define MCHAIN_ATTR_MIRROR           (1u << 6)
#define MCHAIN_ATTR_DRIVER           (1u << 7)
#define MCHAIN_ATTR_MASK             0xfffffff0u

/* 特殊值 */
#define MCHAIN_NID_ANY               (-1)
#define MCHAIN_ANY_ADDR              ((phys_addr_t)~(phys_addr_t)0)

/* 命名预留 */
#define MCHAIN_NAME_SLOTS            32
#define MCHAIN_NAME_MAX              32
#define MCHAIN_NAME_NONE             0u

/* 迭代器，字段语义见 §14.7 */
struct mchain_iter {
    int          cur;
    phys_addr_t  min_addr;
    phys_addr_t  max_addr;
    int          nid;
    unsigned int attr_mask;
};

/* 调试统计，见 §14.8 */
struct mchain_stats {
    unsigned int used_entries;
    unsigned int peak_used_entries;
    unsigned int capacity;
    unsigned int grow_count;
    unsigned int alloc_failures;
    unsigned int frozen;
    phys_size_t  free_bytes;
    phys_size_t  reserved_bytes;
    phys_size_t  allocated_bytes;
};
```

约定：

- `MCHAIN_STATE_*` 与 `MCHAIN_ATTR_*` 共同存放在 `struct mchain_entry.flags` 中；公开 API 的 `attrs` 参数只接受 `MCHAIN_ATTR_MASK` 中的位，传入状态位返回 `-EINVAL`。
- `MCHAIN_ATTR_DRIVER` 表示由驱动管理的区域；普通分配和页分配器交接默认应跳过它。只有在 `attr_mask` 中显式要求时，分配器和迭代器才返回这类区域。
- `MCHAIN_ANY_ADDR` 作为分配 API 的 `min_addr` 时表示使用 `min_limit`，作为分配 API 的 `max_addr` 时表示使用 `max_limit`；在迭代器中，`MCHAIN_ANY_ADDR` 表示不设该侧边界。
- `MCHAIN_NID_ANY` 既可用于要求“任意节点”，也可用于标记“该区域的 NUMA 归属无关”。
- 所有地址区间都是半开区间 `[base, base + size)`，所有 `size` 的单位都是字节。

### 14.2 API 总览

公共 API 声明汇总：

```c
/* 生命周期与分配方向 */
int          mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
void         mchain_set_bottom_up(bool bottom_up);
bool         mchain_bottom_up(void);
void         mchain_allow_grow(void);
void         mchain_freeze(void);

/* 区域管理 */
int          mchain_add(phys_addr_t base, phys_size_t size,
                        unsigned int attrs, int nid);
int          mchain_reserve(phys_addr_t base, phys_size_t size,
                            unsigned int attrs, int nid);
int          mchain_unreserve(phys_addr_t base, phys_size_t size);

/* 命名预留 */
int          mchain_reserve_named(const char *name, phys_addr_t base,
                                  phys_size_t size, unsigned int attrs, int nid);
int          mchain_find_reserved(const char *name, phys_addr_t *base,
                                  phys_size_t *size);

/* 查找、分配与释放 */
phys_addr_t  mchain_find_in_range(phys_size_t size, phys_size_t align,
                                  phys_addr_t min_addr, phys_addr_t max_addr,
                                  int nid, unsigned int attr_mask);
phys_addr_t  mchain_alloc(phys_size_t size, phys_size_t align,
                          phys_addr_t min_addr, phys_addr_t max_addr,
                          int nid, unsigned int attr_mask);
int          mchain_free(phys_addr_t base, phys_size_t size);

/* 查询 */
bool         mchain_range_is_free(phys_addr_t base, phys_size_t size);
bool         mchain_is_memory(phys_addr_t addr);
phys_size_t  mchain_free_size(void);
phys_size_t  mchain_reserved_size(void);
phys_size_t  mchain_memory_size(void);
phys_addr_t  mchain_start_of_dram(void);
phys_addr_t  mchain_end_of_dram(void);
unsigned int mchain_used_entries(void);
unsigned int mchain_capacity(void);

/* 迭代器 */
void         mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
                              phys_addr_t max_addr, int nid,
                              unsigned int attr_mask);
int          mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);

/* 调试与统计 */
int          mchain_validate(void);
void         mchain_dump(void);
void         mchain_get_stats(struct mchain_stats *stats);
```

| 类别     | API                                                                | 成功返回         | 失败返回                          |
| -------- | ------------------------------------------------------------------ | ---------------- | --------------------------------- |
| 生命周期 | `mchain_set_limit`                                                 | `0`              | `-EINVAL`                         |
|          | `mchain_set_bottom_up` / `mchain_bottom_up`                        | 无 / 当前方向    | 无                                |
|          | `mchain_allow_grow`                                                | 无               | 无                                |
|          | `mchain_freeze`                                                    | 无               | 无                                |
| 区域管理 | `mchain_add`                                                       | `0`              | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
|          | `mchain_reserve`                                                   | `0`              | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
|          | `mchain_unreserve`                                                 | `0`              | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
| 分配     | `mchain_find_in_range`                                             | 物理地址         | `0`                               |
|          | `mchain_alloc`                                                     | 物理地址         | `0`                               |
|          | `mchain_free`                                                      | `0`              | `-EINVAL` / `-ENOENT` / `-EBUSY`  |
| 查询     | `mchain_range_is_free`                                             | `true` / `false` | 无                                |
|          | `mchain_is_memory`                                                 | `true` / `false` | 无                                |
|          | `mchain_free_size` / `mchain_reserved_size` / `mchain_memory_size` | 字节数           | 无                                |
|          | `mchain_start_of_dram` / `mchain_end_of_dram`                      | 物理地址         | 无                                |
|          | `mchain_used_entries` / `mchain_capacity`                          | 条目数           | 无                                |
| 迭代     | `mchain_iter_init`                                                 | 无               | 无                                |
|          | `mchain_iter_next`                                                 | `1`              | `0`（遍历结束）                   |
| 调试     | `mchain_validate`                                                  | `0`              | `-EINVAL`                         |
|          | `mchain_dump` / `mchain_get_stats`                                 | 无               | 无                                |
| 命名预留 | `mchain_reserve_named`                                             | `0`              | `-EINVAL` / `-ENOMEM` / `-ENOSPC` |
|          | `mchain_find_reserved`                                             | `0`              | `-ENOENT` / `-EINVAL`             |

### 14.3 静态初始化、分配方向与生命周期

#### 静态初始化与 `mchain_bootstrap`

全局池在定义时直接初始化，不需要 `mchain_init()`：

```c
static struct mchain_entry mchain_static_entries[MCHAIN_STATIC_CAPACITY];

struct mchain_pool mchain_pool_instance = {
    .entries         = mchain_static_entries,
    .capacity        = MCHAIN_STATIC_CAPACITY,
    .used            = 0,
    .head            = -1,
    .free_head       = -1,
    .bottom_up       = true,
    .can_grow        = 0,
    .frozen          = 0,
    .free_list_ready = 0,
    .min_limit       = 0,
    .max_limit       = MCHAIN_ANY_ADDR,
};
```

- `head = -1`：区域链表为空。
- `free_head = -1`、`free_list_ready = 0`：静态空闲链表尚未建立。
- `bottom_up = true`：默认 bottom-up。
- `can_grow = 0`、`frozen = 0`：扩容关闭、未冻结。
- 默认分配范围 `[0, MCHAIN_ANY_ADDR)`；可通过 `mchain_set_limit()` 调整。
- **不添加任何内存区域**。调用者随后用 `mchain_add()` 添加固件报告的可用内存，再用 `mchain_reserve()` 预留固件占用区和内核镜像。

`mchain_bootstrap(struct mchain_pool *pool)` 是内部函数，不是公开 API。第一次需要空闲链表的公开 API（`mchain_add()`、`mchain_reserve()`、`mchain_unreserve()`、`mchain_alloc()`、`mchain_free()`，以及 `mchain_validate()` / `mchain_dump()`）会调用 `mchain_bootstrap(&mchain_pool_instance)`：把全部静态条目串成空闲链表，设置 `free_head = capacity ? 0 : -1`、`free_list_ready = 1`。该函数幂等，不依赖页分配器或锁，只遍历静态数组。

在 bootstrap 之前，只读的尺寸/状态查询和 `mchain_iter_*` 看到的是空链表；`mchain_set_limit()`、`mchain_set_bottom_up()`、`mchain_allow_grow()`、`mchain_freeze()` 可以在此之前调用。测试中若要恢复初始状态，需要重新初始化全局 `mchain_pool_instance`、静态 arena 和命名表；生产代码不应重新初始化。

#### `mchain_set_limit`

```c
int mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
```

调整全局分配上下限，只影响后续的 `mchain_find_in_range()` / `mchain_alloc()`，不修改任何区域条目。

- `min_addr` 是包含下界，`max_addr` 是开区间上界。
- `max_addr == MCHAIN_ANY_ADDR` 表示不设上限；`min_addr == MCHAIN_ANY_ADDR` 视为 0。
- `min_addr >= max_addr` 返回 `-EINVAL`，原有设置保持不变。
- 该 API 用于启动过程中可寻址范围扩大或缩小的场景；默认限制为 `[0, MCHAIN_ANY_ADDR)`。

#### `mchain_set_bottom_up` / `mchain_bottom_up`

```c
void mchain_set_bottom_up(bool bottom_up);
bool mchain_bottom_up(void);
```

- `mchain_set_bottom_up(true)` 选择 bottom-up：`mchain_find_in_range()` / `mchain_alloc()` 从链表头向尾扫描。
- `mchain_set_bottom_up(false)` 选择 top-down：从链表尾沿 `prev` 向头扫描。
- 默认值为 `true`，与 memblock 一致。
- 设置只影响后续查找和分配，不改变已有区域；可以在任何分配之前调用。
- `mchain_bottom_up()` 返回当前方向。
- 参考 memblock 的 `memblock_set_bottom_up()` / `memblock_bottom_up()`，不再给 `mchain_alloc()` 传 `direction` 参数。

#### `mchain_allow_grow`

```c
void mchain_allow_grow(void);
```

开启 arena 自动扩容。只能在以下条件满足后调用：

- 最终直接映射已经建立，`phys_to_virt()` 能访问任意新分配的 arena。
- 内核镜像、静态 arena 所在区域已经被预留，不会被后续分配拿走。

首次调用前 `can_grow == 0`，静态条目耗尽时 `mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` 返回 `-ENOMEM`，`mchain_alloc()` 返回 `0`。该函数幂等，可在启动流程中多次调用。

#### `mchain_freeze`

```c
void mchain_freeze(void);
```

冻结分配器，用于页分配器交接前。该函数幂等。

- 置 `frozen = 1`，`can_grow = 0`。
- `mchain_alloc()` / `mchain_find_in_range()` 返回 0。
- `mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` / `mchain_free()` 返回 `-EBUSY`。
- `mchain_allow_grow()` 在冻结后为空操作。
- 查询、`mchain_iter_*`、`mchain_dump()` / `mchain_validate()` / `mchain_get_stats()` 仍然可用，以便完成交接和事后审计。
- 冻结不可逆；要重新使用分配器只能重新初始化全局池（仅测试场景）。
- 交接顺序应为：先 `mchain_freeze()`，再遍历 `FREE` 区间交给伙伴系统；这样遍历过程中不会有新的 mchain 分配从同一区间取走内存。
- 冻结后 `FREE` 条目仍保留在链表中，供交接迭代器使用；交接完成后这些区间已经属于伙伴系统，查询结果只用于审计，不能再据此做 mchain 分配。
- 需要在交接时交给伙伴系统的 `RESERVED` 区间必须先 `mchain_unreserve()`，需要释放的 `ALLOCATED` 区间必须先 `mchain_free()`；冻结后不再接受释放操作。

### 14.4 区域管理 API

#### `mchain_add`

```c
int mchain_add(phys_addr_t base, phys_size_t size,
               unsigned int attrs, int nid);
```

向分配器登记一段可用物理内存。

- 只插入 `MCHAIN_STATE_FREE` 条目，`attrs` 为这段内存的属性（如 `MCHAIN_ATTR_HOTPLUG`）。
- 采用 memblock 风格的“已有条目优先”语义：目标范围中已经被任何条目覆盖的字节保持不变，只把尚未覆盖的间隙以 `FREE | attrs` 插入。因此重复添加、部分重叠添加都是安全的。
- 新插入的 `FREE` 条目会与相邻的、`attrs` / `nid` 相同的 `FREE` 条目合并；这可能会扩展一个已有 `FREE` 条目，但不会改写已覆盖字节的属性。
- `size == 0` 是成功的空操作。
- `base + size` 回绕、`attrs` 含非法位时返回 `-EINVAL`。
- 需要新条目但静态 arena 已满且扩容未开启/失败时返回 `-ENOMEM`；目标范围不会发生部分添加。若触发扩容，扩容会避开 `[base, base + size)`，不会把本次要添加的内存拿去当 arena；但扩容本身可能已经消耗了其他空闲内存。
- 本函数不负责预留内核镜像；调用者应在添加完可用内存后显式调用 `mchain_reserve()`。
- `mchain_freeze()` 之后返回 `-EBUSY`。

示例：

```c
mchain_add(0x80000000, 0x10000000, 0, 0);
mchain_add(0x90000000, 0x10000000, 0, 0);  /* 两段相邻，合并为一段 FREE */
```

#### `mchain_reserve`

```c
int mchain_reserve(phys_addr_t base, phys_size_t size,
                   unsigned int attrs, int nid);
```

将目标范围内的空闲内存转为预留。

- 只处理已经由 `mchain_add()` 登记为 `FREE` 的字节：对每个与 `[base, base + size)` 相交的空闲子区间，切除后以 `MCHAIN_STATE_RESERVED | (源属性 | attrs)` 插入；如果子区间正好覆盖整个空闲条目，也可以直接原地改状态，避免多占一个条目。尚未添加的空洞不会被记录为预留，因此必须先添加完可用内存再预留。
- 已经是 `RESERVED` 或 `ALLOCATED` 的字节保持原状，不会被覆盖、拆分或改变属性。因此允许部分重叠，也允许对同一范围重复预留。
- 新预留的字节**保留源空闲条目的属性**，并把参数 `attrs` 按位或进去。这样 NOMAP / HOTPLUG / MIRROR / DRIVER 等属性不会因为状态从 `FREE` 变成 `RESERVED` 而丢失；`attrs` 只增不减，若要清除属性需要额外的属性修改 API。
- 目标范围全部落在非空闲区域时返回成功但不做任何事。返回 0 并不表示整个范围都变成了 `RESERVED`；需要严格语义的调用者应先使用 `mchain_range_is_free()` 检查。
- `size == 0` 是成功的空操作。
- 参数非法返回 `-EINVAL`；条目不足且无法扩容返回 `-ENOMEM`，目标范围不会留下部分预留。扩容会避开目标范围 `[base, base + size)`，不会从本次要预留的内存中分配新 arena；但扩容本身可能已经消耗了其他空闲内存。
- `mchain_reserve()` 的目标范围操作是原子的：不会出现“切除了一部分但预留插入失败”的中间状态。
- `mchain_freeze()` 之后返回 `-EBUSY`。

#### `mchain_unreserve`

```c
int mchain_unreserve(phys_addr_t base, phys_size_t size);
```

将目标范围内的预留内存转回空闲。

- 只处理 `RESERVED` 字节；`FREE` 和 `ALLOCATED` 字节保持不变。
- 支持部分释放，即使多个预留区已经合并成一个较大的 `RESERVED` 条目，也可以只释放其中一段。若释放区间正好覆盖整个预留条目，直接原地改成 `FREE`，不需要额外条目。
- 释放出的字节以 `FREE` 状态重新插入，保留原预留条目的 `attrs` 和 `nid`；释放出的 `FREE` 字节的 `name_id` 必须置为 `MCHAIN_NAME_NONE`，名字只属于仍处于 `RESERVED` 状态的部分。随后与相邻空闲条目合并。
- 目标范围全部不是预留内存时返回成功但不做任何事。
- `size == 0` 是成功的空操作。
- 参数非法返回 `-EINVAL`；分裂需要条目但无法预分配时返回 `-ENOMEM`，目标范围不会留下部分释放。扩容会避开目标范围，但可能已经消耗了其他空闲内存作为 arena。
- 本函数不释放 `ALLOCATED` 条目；释放分配必须用 `mchain_free()`。
- `mchain_freeze()` 之后返回 `-EBUSY`。

#### 命名预留

```c
int mchain_reserve_named(const char *name, phys_addr_t base, phys_size_t size,
                         unsigned int attrs, int nid);
int mchain_find_reserved(const char *name, phys_addr_t *base, phys_size_t *size);
```

- `mchain_reserve_named()` 等价于 `mchain_reserve()`，并额外把 `name` 登记到静态字符串表，写入条目的 `name_id`。
- 实现必须先在名字表中查找或登记名字，再执行预留；名字表满时返回 `-ENOSPC`，此时尚未修改区域链表。若随后的预留因 `-ENOMEM` 失败，名字表中可能留下一个未被引用的名字，但不会留下部分预留。
- `name_id` 参与合并条件（§9），不同名字的相邻预留不会合并。
- `mchain_find_reserved()` 查找第一个同名预留，成功返回 0 并写出 `base` / `size`；找不到返回 `-ENOENT`，`name == NULL` 返回 `-EINVAL`。
- 本实现已包含这一组 API；`MCHAIN_NAME_NONE == 0` 表示无名。

### 14.5 分配与释放 API

#### `mchain_find_in_range`

```c
phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
                                 phys_addr_t min_addr, phys_addr_t max_addr,
                                 int nid, unsigned int attr_mask);
```

只查找满足条件的空闲区间，不修改链表、不触发扩容。`mchain_freeze()` 之后返回 0，避免调用者在交接后继续规划 mchain 分配。

参数：

| 参数        | 含义                                                                                                                  |
| ----------- | --------------------------------------------------------------------------------------------------------------------- |
| `size`      | 请求字节数，必须大于 0                                                                                                |
| `align`     | 起始地址对齐，必须是大于 0 的 2 的幂；`1` 表示不限制                                                                  |
| `min_addr`  | 允许的下界（含）；`MCHAIN_ANY_ADDR` 表示使用 `min_limit`                                                              |
| `max_addr`  | 允许的上界（不含）；`MCHAIN_ANY_ADDR` 表示使用 `max_limit`                                                            |
| `nid`       | 节点号；`MCHAIN_NID_ANY` 匹配任意节点，具体节点也匹配 `MCHAIN_NID_ANY` 条目                                           |
| `attr_mask` | 必须匹配的属性位，只使用 `MCHAIN_ATTR_MASK` 中的位；`MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER` 默认跳过，除非显式要求 |

扫描方向由全局 `mchain_set_bottom_up()` 决定：`true` 从头向尾，`false` 从尾向头；默认 `true`。

有效范围：

```
lo = max(min_addr == MCHAIN_ANY_ADDR ? 0 : min_addr, pool.min_limit)
hi = min(max_addr == MCHAIN_ANY_ADDR ? MCHAIN_ANY_ADDR : max_addr, pool.max_limit)
```

若 `lo >= hi` 或 `size > hi - lo`，直接返回 0。

候选地址计算：

- **Bottom-up**：在空闲条目 `[b, e)` 内，`t = min(e, hi)`，
  `c = align_up(max(b, lo), align)`；要求对齐计算不回绕、`c >= max(b, lo)`、`c + size` 不回绕且 `c + size <= t`。若 `c == 0`，在同一空闲条目内尝试下一个对齐地址（通常是 `align`）；仍不满足则跳过该条目。
- **Top-down**：`t = min(e, hi)`；要求 `t >= size`，
  `c = align_down(t - size, align)`；要求 `c >= max(b, lo)` 且 `c != 0`。`c == 0` 时没有更低的候选地址，直接跳过该条目。

返回第一个满足条件的 `c`；没有则返回 0。物理地址 0 永远不作为成功结果返回。

#### `mchain_alloc`

```c
phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
                         phys_addr_t min_addr, phys_addr_t max_addr,
                         int nid, unsigned int attr_mask);
```

查找规则与 `mchain_find_in_range()` 完全相同，成功时按以下顺序执行：

1. 先找到候选 `candidate` 及其所在的空闲条目，但**不要修改链表**。
2. 根据候选在条目中的位置计算需要的额外条目数：覆盖整个空闲条目时需要 0 个；贴住条目左端或右端时需要 1 个；位于条目中间时需要 2 个。不要一律按 2 个预分配，否则 arena 只剩少量槽位时可能不必要地扩容甚至失败。候选搜索必须带条目预算：`needed` 不能超过当前 `capacity - used`；预算不足且无法通过扩容解决的候选应跳过，继续向后搜索。
3. 若 `needed` 超过当前空闲条目数，调用 `mchain_ensure_entries(pool, needed, candidate, candidate + size)`。扩容会避开候选区间，因此不会把本次要分配的内存拿去做 arena。若预分配触发了扩容，链表结构可能变化，必须重新执行第 1 步查找候选并重新计算 `needed`；若仍不足，重复本步骤，且继续避开新的候选区间。**扩容失败或重新查找失败时不要立即返回 0**：继续搜索后面满足当前条目预算的候选（例如 arena 只剩 1 个槽位时，贴边候选仍然可用），一个可行候选都没有时才返回 0。
4. 从选中的空闲条目中切除精确的 `[candidate, candidate + size)`。若候选覆盖整个空闲条目，直接原地把该条目改成 `ALLOCATED`；否则复用原条目承载一个片段，只对真正新增的片段从空闲链表取条目。
5. 把分配区间标记为 `MCHAIN_STATE_ALLOCATED`，**继承源空闲条目的全部属性与 `nid`**；`attr_mask` 只用于筛选，不会被用来截断属性。
6. 返回 `candidate`。

行为约定：

- 只分配精确的请求大小，不吞并对齐产生的左侧/右侧碎片（§6.1）。因此 `mchain_free(candidate, size)` 一定能精确匹配这条分配。
- `ALLOCATED` 条目永不与相邻条目合并，即使属性、`nid` 和地址都连续。
- `mchain_find_in_range()` 只做地址/属性筛选，不检查 arena 条目预算；`mchain_alloc()` 额外应用上述预算规则。关闭扩容且 arena 只剩 1 个槽位时，整段或贴边候选仍应成功，不能因为链表中第一个候选位于中间就整体失败。
- `size == 0`、`align` 非 2 的幂、`nid < MCHAIN_NID_ANY`、参数导致地址区间回绕时返回 0。
- `attr_mask` 只使用属性位；实现应把它与 `MCHAIN_ATTR_MASK` 相与，状态位不参与匹配。
- 分配条目的 `nid` 继承源空闲条目；请求的 `nid` 只用于筛选，不会把一段 `MCHAIN_NID_ANY` 的物理内存改写成具体节点。若需要节点绑定，应在 `mchain_add()` / `mchain_reserve()` 时写入具体 `nid`。
- 没有满足条件的空闲区间、条目不足且扩容失败时返回 0。
- `mchain_freeze()` 之后返回 0。
- 失败时目标分配不会留下部分状态。若预分配触发了扩容，扩容会消耗一段空闲内存作为 arena 并登记为 `ALLOCATED`；这是失败调用可能留下的副作用，不是目标分配的一部分。
- 每次失败递增 `alloc_failures`，用于统计。

#### `mchain_free`

```c
int mchain_free(phys_addr_t base, phys_size_t size);
```

释放一次由 `mchain_alloc()` 返回的分配。

- 在链表中查找 `state == MCHAIN_STATE_ALLOCATED` 且 `base`、`size` 都完全相等的条目。
- 找到后将其状态改为 `FREE`，保留原属性和 `nid`，并与相邻的、键相同的空闲条目合并。
- 部分释放不支持：不能只释放分配的一半，也不能用不同的 `size` 释放。由于 `ALLOCATED` 条目永不合并，每次分配的边界始终是精确的。
- `size == 0` 返回 `-EINVAL`。
- 找不到完全匹配的 `ALLOCATED` 条目返回 `-ENOENT`；目标范围是 `RESERVED` 时同样返回 `-ENOENT`，应改用 `mchain_unreserve()`。
- `base + size` 回绕返回 `-EINVAL`。
- 释放不需要新条目，不会触发扩容。
- `mchain_freeze()` 之后返回 `-EBUSY`。

### 14.6 查询 API

```c
bool mchain_range_is_free(phys_addr_t base, phys_size_t size);
bool mchain_is_memory(phys_addr_t addr);

phys_size_t mchain_free_size(void);
phys_size_t mchain_reserved_size(void);
phys_size_t mchain_memory_size(void);

phys_addr_t mchain_start_of_dram(void);
phys_addr_t mchain_end_of_dram(void);

unsigned int mchain_used_entries(void);
unsigned int mchain_capacity(void);
```

| API                    | 语义                                                                                                              |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `mchain_range_is_free` | 当且仅当 `[base, base + size)` 的每个字节都属于 `FREE` 条目时返回 true；`size == 0` 返回 true；区间回绕返回 false |
| `mchain_is_memory`     | 当且仅当 `addr` 被任意状态的条目覆盖时返回 true；用于 `pfn_valid()` 等查询                                        |
| `mchain_free_size`     | 所有 `FREE` 条目的字节数之和                                                                                      |
| `mchain_reserved_size` | 所有 `RESERVED` 与 `ALLOCATED` 条目的字节数之和                                                                   |
| `mchain_memory_size`   | 所有条目（三种状态）的字节数之和                                                                                  |
| `mchain_start_of_dram` | 链表第一个条目的 `base`；链表为空返回 0（空链表与真实地址 0 无法区分，可用 `mchain_memory_size() == 0` 判断）     |
| `mchain_end_of_dram`   | 链表最后一个条目的 `base + size`；链表为空返回 0                                                                  |
| `mchain_used_entries`  | 当前区域链表中的条目数                                                                                            |
| `mchain_capacity`      | 当前 arena 容量                                                                                                   |

在无溢出时满足：

```
mchain_free_size() + mchain_reserved_size() == mchain_memory_size()
```

这些函数只读，不触发扩容，也不会修改链表。注意 `mchain_range_is_free()` 只判断状态是否为 `FREE`，不考虑 `MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER` 等属性；迭代器和分配器默认会跳过这些区域，两者语义不同。

### 14.7 迭代器 API

```c
void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr, phys_addr_t max_addr,
                      int nid, unsigned int attr_mask);
int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);
```

`mchain_iter_init()`：

- 初始化迭代器，`cur` 指向链表头。
- `min_addr == MCHAIN_ANY_ADDR` 表示不设下界；`max_addr == MCHAIN_ANY_ADDR` 表示不设上界。
- `min_addr >= max_addr` 时迭代器立即结束。
- `nid` / `attr_mask` 的匹配规则与 `mchain_alloc()` 相同，包括 `MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER` 默认跳过的规则。

`mchain_iter_next()`：

- 成功找到一个区间时，写入 `*start` / `*end` 并返回 1；遍历结束返回 0。
- 只返回 `FREE` 条目与 `[min_addr, max_addr)` 的交集；非 `FREE`、`nid` 不匹配、属性不匹配、无交集的条目被跳过。
- 每个条目最多产生一个区间，因此不需要条目内偏移字段。
- 输出指针不能为 NULL，调试构建应断言。
- 迭代期间不能调用任何修改链表的 API；需要修改时先结束迭代并重新初始化。

典型用法：

```c
struct mchain_iter it;
phys_addr_t start, end;

mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
while (mchain_iter_next(&it, &start, &end))
        free_range_to_buddy(start, end);
```

### 14.8 调试与统计 API

```c
int mchain_validate(void);
void mchain_dump(void);
void mchain_get_stats(struct mchain_stats *stats);
```

- `mchain_validate()`：先检查 `free_list_ready == 0` 时的前置条件（`head == -1`、`free_head == -1`、`used == 0`），再确保 `mchain_bootstrap(&mchain_pool_instance)` 已执行，然后检查 §14.12 的全部不变量。成功返回 0，发现不一致返回 `-EINVAL`。复杂度 O(n)，仅供调试构建或启动后期使用；不修改区域链表。
- `mchain_dump()`：先确保 `mchain_bootstrap(&mchain_pool_instance)` 已执行，再按链表顺序输出每个条目的 `base`、`size`、状态名、属性名、`nid`、`name_id`、下标以及 `prev` / `next`，并输出 arena 基址、容量、`used`、`free_head`。使用内核日志接口，不分配内存。
- `mchain_get_stats()`：把当前统计复制到 `stats`；`stats == NULL` 时为空操作。字段含义：
  - `used_entries`：当前链表中条目数。
  - `peak_used_entries`：历史峰值。
  - `capacity`：当前 arena 容量。
  - `grow_count`：成功扩容次数。
  - `alloc_failures`：`mchain_alloc()` 累计失败次数。
  - `frozen`：是否已经通过 `mchain_freeze()` 冻结。
  - `free_bytes` / `reserved_bytes` / `allocated_bytes`：分别只统计 `FREE`、`RESERVED`、`ALLOCATED` 三种状态的字节数。把 `reserved_bytes + allocated_bytes` 相加才对应 `mchain_reserved_size()`。

调试构建建议在每次修改后执行：

```c
assert(mchain_validate() == 0);
```

### 14.9 内部 API

以下函数不对外公开，仅供 `mchain.c` 和单元测试使用。调用者必须保证单线程、参数合法，并已按契约完成条目预分配或加锁（若未来引入锁）。

```c
void mchain_bootstrap(struct mchain_pool *pool);
int mchain_ensure_entries(struct mchain_pool *pool, unsigned int nr,
                          phys_addr_t avoid_start, phys_addr_t avoid_end);
int mchain_insert_entry(struct mchain_pool *pool, phys_addr_t base,
                        phys_size_t size, unsigned int state,
                        unsigned int attrs, int nid, unsigned int name_id);
int mchain_carve_range(struct mchain_pool *pool, phys_addr_t start,
                       phys_addr_t end, unsigned int state,
                       unsigned int attr_mask, int nid, phys_size_t *carved);
int mchain_grow_pool(struct mchain_pool *pool, phys_addr_t avoid_start,
                     phys_addr_t avoid_end);
void mchain_unlink_entry(struct mchain_pool *pool, int idx);
int mchain_merge_neighbors(struct mchain_pool *pool, int idx);
```

| 函数                     | 契约                                                                                                                                                                                                                                              |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `mchain_bootstrap`       | 若 `pool->free_list_ready == 0`，把 `pool->entries` 中的全部条目串成空闲链表并置 `free_head = pool->capacity ? 0 : -1`、`free_list_ready = 1`；幂等，不分配内存。公开 API 传入 `&mchain_pool_instance`，单元测试可传入自己的 pool                 |
| `mchain_ensure_entries`  | 确保至少有 `nr` 个空闲条目；不足且 `can_grow == 1` 时调用 `mchain_grow_pool()`，扩容区间不得与 `[avoid_start, avoid_end)` 相交。成功返回 0；失败返回 `-ENOMEM`，此时目标 `avoid` 范围未被修改，但可能已经完成了部分扩容并消耗了其他空闲内存       |
| `mchain_insert_entry`    | 插入一个不与任何条目重叠的区域；调用者必须已保证有空闲条目。`state` 必须是 `FREE` / `RESERVED` / `ALLOCATED` 之一，`attrs` 只含属性位；只有 `RESERVED` 可以带非零 `name_id`；按 §4.2 合并；成功返回 0，参数非法返回 `-EINVAL`                     |
| `mchain_carve_range`     | 从匹配 `state` / `attr_mask` / `nid` 的条目中挖去 `[start, end)`；不匹配的条目保持不变。调用者必须按每个相交条目精确计算并预分配额外条目（最多 2 个）。成功返回 0 并通过 `carved` 返回切除字节数，失败返回 `-ENOMEM` / `-EINVAL` 且不留下部分修改 |
| `mchain_grow_pool`       | 按 §7.2 的自举流程扩容，并在查找空闲区间时避开 `[avoid_start, avoid_end)`；成功返回 0，找不到合适物理区间返回 `-ENOMEM`。调用前要求 `can_grow == 1`，函数内部临时置 0，成功和失败都必须恢复为 1                                                   |
| `mchain_unlink_entry`    | 从区域链表摘除指定条目、修正前后链接并压入空闲链表；不处理合并                                                                                                                                                                                    |
| `mchain_merge_neighbors` | 按 §4.2 尝试把指定条目与前后条目合并；返回合并后条目下标。`ALLOCATED` 条目直接返回原下标                                                                                                                                                          |

`mchain_insert_entry()` 已包含 `name_id` 参数（普通区域传 `MCHAIN_NAME_NONE`），`mchain_merge_neighbors()` 的合并判断也比较 `name_id`。其余静态辅助函数（如 free list 的 push/pop、候选地址计算、重叠检查、下标合法性检查）属于实现细节，不构成 API。

### 14.10 错误码与通用约定

| 返回值    | 适用场景                                                                                                                                                                         |
| --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0`       | `int` API 成功；分配类 `phys_addr_t` API 失败（地址 0 为失败哨兵）。查询类 API 的 0 是合法返回值                                                                                 |
| `-EINVAL` | 参数非法：`base + size` 回绕、`align` 不是 2 的幂、`nid < MCHAIN_NID_ANY`、`attrs` / `attr_mask` 含非法位、状态值非法、`min_addr >= max_addr`、`size == 0` 且该 API 不接受空操作 |
| `-ENOMEM` | 需要新条目但空闲条目不足，且扩容未开启或扩容失败                                                                                                                                 |
| `-EBUSY`  | 分配器已通过 `mchain_freeze()` 冻结，拒绝区域修改或分配                                                                                                                          |
| `-ENOENT` | `mchain_free()` 找不到完全匹配的 `ALLOCATED` 条目；`mchain_find_reserved()` 找不到名字                                                                                           |
| `-ENOSPC` | 命名预留的静态名字表已满                                                                                                                                                         |
| `1` / `0` | `mchain_iter_next()` 分别表示“返回一个区间”和“遍历结束”                                                                                                                          |

通用规则：

- 全局池在定义时静态初始化，没有 `mchain_init()`。首次需要空闲链表的修改 API 会调用内部 `mchain_bootstrap()`；在此之前查询/迭代看到空链表。
- `mchain_find_in_range()` 和 `mchain_alloc()` 用 0 表示失败，无法区分“参数非法”和“没有合适区间”。需要诊断时结合 `mchain_validate()`、`mchain_get_stats()` 和调用参数检查。
- `mchain_alloc()` 永不返回物理地址 0；如果一段空闲内存只包含地址 0 且无法对齐到非零地址，它会被跳过。
- 公开 API 不睡眠、不阻塞、不调用 slab / 伙伴系统 / vmalloc；扩容只使用 `phys_to_virt()` 和普通内存拷贝。
- mchain 不是 SMP 安全的。启动早期由单核调用；若将来在多核或中断上下文中使用，必须在调用者侧加锁。
- 修改链表的 API 会使所有未完成的 `struct mchain_iter` 失效。`mchain_freeze()` 之后不能再修改链表，因此可以在冻结后安全地完成最后一次迭代。
- 所有修改 API 的目标区域操作是原子的：要么完整生效，要么目标范围不发生部分修改。扩容是独立的内部操作，可能已经消耗一段空闲内存作为 arena 并登记为 `ALLOCATED`；调用者不能假设失败后 `mchain_free_size()` 完全不变。
- 调试断言不属于 API 契约；生产构建不得依赖断言捕获调用者错误，仍应返回错误码。

### 14.11 启动期调用示例

```c
static void __init mchain_early_setup(void)
{
        phys_addr_t start, end, p, dram_start, kernel_start, dtb_start;
        phys_size_t kernel_size, dtb_size;
        unsigned int reserved_attrs;
        struct mchain_iter it;

        /* 1. 全局池已静态初始化；这里按需设置限制和方向 */
        mchain_set_limit(0, MCHAIN_ANY_ADDR);
        mchain_set_bottom_up(true);

        /* 2. 先添加固件报告的可用内存 */
        for_each_fdt_memory_range(start, end)
                mchain_add(start, end - start, 0, 0);
        dram_start = mchain_start_of_dram();

        /* 3. 再预留固件占用区、内核镜像、DTB、initrd 等。
         *    对 no-map 节点，reserved_attrs 应包含 MCHAIN_ATTR_NOMAP。
         */
        reserved_attrs = 0;
        for_each_fdt_reserved_range(start, end)
                mchain_reserve(start, end - start, reserved_attrs, 0);
        if (dram_start < kernel_start)
                mchain_reserve(dram_start, kernel_start - dram_start, 0, 0);
        mchain_reserve(kernel_start, kernel_size, 0, 0);
        mchain_reserve(dtb_start, dtb_size, 0, 0);

        /* 4. 直接映射建立后允许 arena 扩容 */
        mchain_allow_grow();

        /* 5. 正常分配；失败返回 0 */
        p = mchain_alloc(PAGE_SIZE, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
                         MCHAIN_NID_ANY, 0);
        if (!p)
                panic("mchain: early allocation failed");

        /* 6. 释放必须使用分配时的精确大小 */
        mchain_free(p, PAGE_SIZE);

        /* 7. 冻结分配器，防止交接过程中又有新分配 */
        mchain_freeze();

        /* 8. 交接给伙伴系统：只消费普通 FREE 区间 */
        mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
        while (mchain_iter_next(&it, &start, &end))
                free_range_to_buddy(start, end);

        assert(mchain_validate() == 0);
        mchain_dump();
}
```

全局池已在定义时静态初始化，没有 `mchain_init()`；第一次 `mchain_add()` 会通过内部 `mchain_bootstrap()` 建立空闲链表。注意调用顺序：`mchain_add()` 只添加可用内存，`mchain_reserve()` 只从已经存在的空闲内存中划走预留。必须**先添加完所有可用内存，再执行预留**；如果先预留再添加，落在空洞中的预留调用会被忽略，后添加的 `FREE` 不会自动变成预留。内核镜像和静态 arena 必须在内核开始使用 mchain 分配之前完成预留。调用 `mchain_freeze()` 后，mchain 不再拥有任何可分配内存，后续应完全由伙伴系统接管。

### 14.12 不变量

`mchain_validate()` 检查以下条件：

1. `entries != NULL`，`capacity > 0`，`used <= capacity`。
2. `head == -1` 当且仅当链表为空；`free_list_ready` 为 0 或 1。若为 0，则 `head == -1`、`free_head == -1` 且 `used == 0`；若为 1，则 `free_head == -1` 当且仅当没有空闲条目。`mchain_validate()` 在 bootstrap 之前先检查这个前置条件，然后才建立空闲链表，因此最终校验时该字段应为 1。
3. 区域链表的下标全部在 `[0, capacity)` 内，无环。
4. 链表按 `base` 严格升序，条目之间不重叠，且 `base + size` 不回绕。
5. 对每个条目，`prev` / `next` 互为逆关系：`entries[i].next != -1` 时 `entries[entries[i].next].prev == i`，反之亦然；链表头 `prev == -1`，尾 `next == -1`。
6. 链表长度等于 `used`。
7. 每个链表条目的 `flags & MCHAIN_STATE_MASK` 恰好是 `FREE` / `RESERVED` / `ALLOCATED` 之一；`flags & ~(MCHAIN_STATE_MASK | MCHAIN_ATTR_MASK)` 为 0（保留位未使用）；`nid >= MCHAIN_NID_ANY`；只有 `RESERVED` 条目可以带非零 `name_id`，`FREE` / `ALLOCATED` 条目的 `name_id` 必须为 `MCHAIN_NAME_NONE`。
8. 任意相邻条目都不满足 §4.2 的合并条件（即不存在本应合并却未合并的 `FREE` / `RESERVED` 对；`ALLOCATED` 已由合并条件排除）。
9. 当 `free_list_ready == 1` 时，空闲链表中的下标全部在 `[0, capacity)` 内，无重复、无环，且与区域链表不相交；空闲链表长度等于 `capacity - used`。
10. `peak_used >= used`；`grow_count` / `alloc_failures` 只增不减。
11. `mchain_free_size() + mchain_reserved_size() == mchain_memory_size()`（在无溢出前提下）。
12. `can_grow`、`frozen` 和 `free_list_ready` 只能是 0 或 1；扩容过程中 `can_grow == 0`；`frozen == 1` 时 `can_grow == 0`。
13. `bottom_up` 只能是 `true` 或 `false`；它只影响后续查找/分配，不影响已有条目。

任何修改 API 在调试构建返回前都应满足这些不变量。生产构建可以编译掉 `mchain_validate()` 调用，但不能改变 API 的语义。
