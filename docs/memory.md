# 内存管理

本文描述 BRK 的物理内存发现、页分配器、内核堆、虚拟内存、用户地址空间、mmap/COW 和页缓存。启动阶段的内存初始化顺序见 [boot.md](boot.md)。

相关源码：

- `src/include/brk/memblock.h`、`pgalloc.h`、`slab.h`、`kmalloc.h`、`vmalloc.h`、`ioremap.h`
- `src/include/brk/mm_types.h`、`mm.h`、`pagecache.h`、`pgtable.h`
- `src/mm/memblock.c`、`pgalloc.c`、`slab.c`、`kmalloc.c`、`vmalloc.c`
- `src/mm/mm.c`、`mmap.c`、`pagecache.c`
- `src/arch/riscv/mm/init.c`、`page_table.c`
- `src/arch/riscv/include/arch/mm.h`、`arch/pgtable.h`

## 1. 地址空间布局

| 区域 | 起始地址 | 大小/说明 |
| --- | --- | --- |
| 用户空间 | `0` | `USER_SPACE_SIZE_MAX = 0x3ffffff000` |
| 线性映射 | `PAGE_OFFSET = 0xffffffc000000000` | 128 GiB |
| vmalloc | `VMALLOC_START = 0xffffffe000000000` | 64 GiB |
| vmemmap | `VMEMMAP_START = 0xfffffff000000000` | 4 GiB |
| 内核镜像 | `KERNEL_LINK_ADDR = 0xffffffff80000000` | 镜像大小决定 |

重要转换：

```c
phys_to_virt(paddr) = paddr - phys_offset + PAGE_OFFSET;
virt_to_phys(vaddr) = vaddr - PAGE_OFFSET + phys_offset;
symbol_phys(ptr)    = ptr - boot_info.kernel_image_offset;
```

- `phys_to_virt/virt_to_phys` 只适用于线性映射区域；
- 内核镜像符号位于高地址链接区，不是线性映射别名，必须使用 `symbol_phys()`；
- vmalloc/ioremap 区域位于独立虚拟窗口，不是线性映射。

## 2. 物理内存发现：memblock

### 2.1 数据结构

```c
struct memblock_region {
    u64 base;
    size_t size;
};

struct memblock_region_set {
    char const *name;
    struct memblock_region *regions;
    size_t max;
    size_t cnt;
};

struct memblock {
    struct memblock_region_set memory;
    struct memblock_region_set reserved;
};
```

当前使用静态数组：

- `INIT_MEMBLOCK_REGIONS = 32`
- `INIT_MEMBLOCK_RESERVED_REGIONS = 32`

区域按地址排序、去重、合并相邻区间。溢出会 panic。

### 2.2 启动期收集

`paging_init()` 中：

1. `dt_init_scan_mem()`：读取 `/memory` 的 `reg`，加入 memory set；
2. `dt_init_scan_rsvd_mem()`：读取 `/reserved-memory` 子节点并 reserve；
3. reserve 内核镜像：
   ```c
   memblock_reserve(_SKERNEL_PHYS, _KERNEL_SIZE);
   ```
4. reserve DTB；
5. `phys_offset = memblock_start_of_dram()`。

### 2.3 分配与移交

- `memblock_alloc(size, min_addr, align)`：在未保留区域中找第一个满足对齐和地址约束的区间，reserve 后返回物理地址；
- `memblock_free(base, size)`：拆分/缩小 reserved 区间；
- `memblock_free_all()`：把所有未保留页交给伙伴分配器。

`memblock_free_all()` 会先快照 free range，再逐个 `memblock_free_range()`：

- 页对齐裁剪；
- 选择自然对齐的最大块；
- 标记块头 `struct page`；
- 调用 `page_free(head, order)`。

之后 memblock 的 free range 为空，因此 `memblock_alloc()` 只适合启动期使用。

### 2.4 遍历

- `for_each_mem_range()`：遍历未被 reserved 覆盖的物理区间；
- `for_each_mem_pfn_range()`：遍历所有 `/memory` 区间，用于 vmemmap 建立。

## 3. 页表

### 3.1 早期页表

`src/arch/riscv/mm/init.c`：

- 使用 `early_pgdir_rsvd_pages` 中预留的 8 页；
- 建立内核镜像物理恒等映射、高地址链接映射、DTB 映射；
- `map_pages()` 自动选择 1 GiB / 2 MiB / 4 KiB 页；
- 页表页由 `alloc_early_pgtable()` 顺序分配；
- `make_satp()` 使用 Sv39。

### 3.2 最终内核页表

`final_pgtable_init()` 构建 `kernel_pgdir`，映射：

- text：`PTE_R | PTE_X`
- rodata：`PTE_R`
- data：`PTE_R | PTE_W`
- bss：`PTE_R | PTE_W`
- DTB：线性映射地址
- 每个未保留物理内存区间：线性映射 `PTE_R | PTE_W`
- 临时 memblock 区间数组本身

页表页通过 `memblock_alloc()` 分配，使用 `phys_to_virt()` 访问。

`load_final_page_table()`：

```c
write_satp(make_satp(symbol_phys(kernel_pgdir)));
sfence_vma();
```

不要对 `kernel_pgdir` 使用 `virt_to_phys()`。

### 3.3 vmemmap

`vmemmap_init()`：

- 对每个 PFN 区间分配连续的 `struct page[]` 物理内存；
- 清零；
- 映射到：
  ```text
  VMEMMAP_START + pfn * sizeof(struct page)
  ```
- `pfn_to_page()` / `page_to_pfn()` 依赖此布局。

### 3.4 用户页表

`create_user_pgtable()`：

- 分配一页作为 PGD；
- 在 `kernel_pgdir_lock` 下复制 `kernel_pgdir`，因此用户页表包含所有内核映射。

`copy_user_pgtable()`：

- 只复制用户 PGD 槽；
- 支持 1 GiB/2 MiB 大页和 4 KiB 页；
- 失败时销毁目标页表。

`destroy_user_pgtable()`：

- 断言没有用户叶子映射；
- 只释放用户页表页。

`switch_pgtable(pgd)`：

- 用于用户页表；
- `pgd` 来自页分配器，可用 `virt_to_phys()`。

`user_access_enable/disable()`：

- 设置/清除 `sstatus.SUM`；
- 内核访问用户地址时由陷阱/调度路径控制。

**限制**：用户页表创建后，内核页表中新增的 vmalloc/ioremap 映射不会自动同步到该用户页表。

## 4. 伙伴页分配器

### 4.1 结构

```c
#define PAGE_ORDER_MAX 12          /* 最大 16 MiB */
#define PCP_MAX_ORDER0 64

struct buddy_bucket {
    struct list_head free_list;
};

struct pcp_cache {
    struct list_head pages;
    unsigned int count;
    spinlock_t lock;
};

static struct buddy_bucket areas[PAGE_ORDER_MAX + 1];
static struct pcp_cache pcps[MAX_CPUS];
```

`struct page` 通过 union 保存 buddy/slab/tmpfs 元数据。页标志：

```c
PAGE_FLAGS_BUDDY
PAGE_FLAGS_HEAD
PAGE_FLAGS_SLAB
PAGE_FLAGS_FREE
PAGE_FLAGS_KMALLOC
PAGE_FLAGS_PCP
```

### 4.2 分配

`page_alloc(order)`：

1. order 0 先尝试当前 CPU 的 PCP 缓存；
2. 否则在 `areas[order..PAGE_ORDER_MAX]` 中找最小可用块；
3. 逐次二分，释放另一半；
4. 初始化页引用计数为 1；
5. 失败时：
   - `pcp_drain_all()`；
   - `page_cache_shrink()`；
   - 重试一次。

`page_zalloc(order)`：分配后清零。

### 4.3 释放

`page_free(pg, order)`：

- 校验 buddy/head/对齐；
- order 0 优先进入 PCP；
- 否则与 buddy 合并，直到不能合并；
- 标记块头、order，加入对应 free list。

`page_get/page_put`：

- 用于 COW 共享页；
- `page_put` 最后一个引用时按 order 0 释放。

### 4.4 锁

- `areas_lock`：保护 buddy free list 和页标志；
- 每 CPU `pcp_cache.lock`：IRQ-safe，保护 PCP 链表；
- `pcp_drain_all()` 获取 `pcp->lock` 后获取 `areas_lock`；
- 不要反向获取。

## 5. Slab、kmalloc、vmalloc

### 5.1 Slab

```c
struct slab_allocator {
    const char *name;
    size_t size;
    size_t align;
    struct list_head slab_list;
    unsigned int page_order;
    spinlock_t lock;
};
```

- 对象元数据放在 `struct page`；
- 空闲对象用对象自身前 `sizeof(void *)` 字节组成侵入式 free list；
- 分配扫描 `slab_list` 找有空闲对象的 slab；
- 没有空闲则 `slab_add_page()`；
- 释放通过地址范围查找所属 slab；
- `slab_alloc_zero()` 分配后清零。

当前 slab 缓存：

- `task_cache`
- `vma_cache`
- `uvms_cache`
- `cached_page_cache`
- inode/dentry/file 缓存
- 10 个 kmalloc size class（8..4096）

### 5.2 kmalloc

- `<= 4096`：走 kmalloc slab；
- `> 4096`：直接 `page_alloc(order)`，并给整个块标记 `PAGE_FLAGS_KMALLOC`；
- `kfree()` 根据页标志判断 slab 或大块；
- `kcalloc/kzalloc` 提供溢出检查和清零。

### 5.3 vmalloc

虚拟区域：

```c
struct vmalloc_region {
    struct list_head list;
    u64 addr;
    size_t size;
    struct page **pages;
    size_t nr_pages;
    bool is_free;
};
```

- `vmalloc_init()` 创建覆盖 `[VMALLOC_START, +VMALLOC_SIZE)` 的初始 free region；
- `find_free_vm_area()` first-fit，必要时拆分区域；
- `vmalloc()` 分配虚拟区间、页数组和物理页，逐页映射；
- `vfree()` 解除映射、释放页、合并相邻 free region；
- `vmalloc_nomap()` 只保留虚拟区间，供 `ioremap()` 使用。

### 5.4 ioremap

`ioremap(paddr, size, flags)`：

- 预留 vmalloc 虚拟区间；
- `map_kernel_pages()` 映射物理地址；
- 返回内核虚拟地址。

`iounmap()` 反向操作。

## 6. 用户地址空间

### 6.1 `uvm_space`

```c
struct uvm_space {
    pgd_t *pgd;
    struct list_head seg;
    struct uvm_region *stack;
    struct uvm_region *heap;
    u64 brk;
    u64 mmap_top;
    refcnt_t refcnt;
};
```

`uvm_space_create()`：

- 分配 `uvm_space`；
- 创建用户页表；
- 初始化 `seg`；
- 分配 stack/heap 区域描述；
- 初始 `brk=0`、`mmap_top=0`。

`uvm_space_destroy()`：

- 遍历 `seg`：unmap、释放页/页缓存引用、释放区域；
- 释放 stack 页和区域；
- 释放 heap 页和区域；
- 销毁用户页表；
- 释放 `uvm_space`。

### 6.2 `uvm_region`

```c
struct uvm_region {
    struct list_head list;
    u64 addr;
    size_t size;
    struct page **pages;
    struct cached_page **cached;
    size_t nr_pages;
    unsigned int flags;
    unsigned int kind;
    struct fs_inode *inode;
    struct fs_file *file;
    loff_t file_offset;
};
```

类型：

- `UVM_REGION_ANON`：匿名页；
- `UVM_REGION_SHARED_FILE`：共享文件映射，持有页缓存引用。

`flags` 是 VMA 的 R/W/X 权限，不是当前 PTE 的真实权限。

### 6.3 mmap 地址选择

`choose_addr()`：

- `MAP_FIXED`：
  - 地址必须非零、页对齐、低于用户上限；
  - 不得低于 heap 起点；
  - 不得与 seg/stack/heap 冲突；
  - 不替换已有映射。
- 非 fixed：
  - 从 `mmap_top`（或 stack 上方）向下分配；
  - 跳过冲突区域；
  - 更新 `mmap_top`。

没有 rbtree，只有线性扫描。

### 6.4 匿名 mmap

`uvm_mmap_anon()`：

- 只支持 `MAP_PRIVATE | MAP_ANONYMOUS`；
- 分配 `uvm_region` 和 `pages` 指针数组；
- 不立即分配物理页/PTE；
- 第一次访问触发 `uvm_handle_fault()`，分配零页并映射。

### 6.5 文件映射

- **私有文件映射**：`uvm_mmap_file_private()` 立即读取文件内容到匿名页，之后按普通 COW 处理，不再持有 inode/file 引用；
- **共享文件映射**：`uvm_mmap_file_shared()`：
  - 要求文件有 page cache；
  - 预先读取每个页并持有 `cached_page`；
  - 直接映射页缓存物理页；
  - VMA 持有 inode/file 引用；
  - 可写映射在扩大文件时先 truncate 到目标大小。

### 6.6 页错误与 COW

`uvm_handle_fault(mm, addr, write, instr)`：

1. 页对齐地址，找到 `seg`/heap/stack 中的区域；
2. 检查 VMA 权限；
3. 如果 PTE 带 `PTE_COW` 且是写错误：
   - 分配新物理页；
   - 复制旧页；
   - 替换区域中的页指针；
   - `page_put(old)`；
   - 写入新 PTE；
4. 共享文件映射使用 `cached_page` 的物理页；
5. 匿名页未分配时分配零页；
6. 映射或更新 PTE；
7. `sfence_vma()`。

`uvm_cow_protect()` 在 fork 时把源 PTE 改为只读 + `PTE_COW`。

### 6.7 mprotect / munmap / mremap / msync

- `mprotect` 只修改 `seg`，会按范围拆分区域；`PROT_NONE` 会先对 COW 页做私有化；
- `munmap` 只处理 `seg`，支持整段/前缀/后缀/中间删除；stack/heap 不可 unmap；
- `mremap` 支持 shrink、原地增长、移动；移动时匿名页复制，共享文件页增加页缓存引用；
- `msync` 只处理共享文件映射，标记/刷新页缓存；BRK 不跟踪硬件 dirty 位，普通写入不会自动标脏。

### 6.8 brk 堆

`task_set_brk()`：

- heap 是独立 `uvm_region`，不在 `seg`；
- 小于 heap 起点：无变化；
- 小于当前 heap end：只更新 `mm->brk`，不释放页；
- 增长：页对齐，检查冲突，分配新页并立即映射 RW；
- 失败回滚新页；
- 新页没有清零（当前实现的一个缺陷）。

### 6.9 exec 内存

`__do_execve()`：

- 新 `uvm_space`；
- 每个 `PT_LOAD` 立即分配/复制/映射页；
- 堆起点 = 最高段末尾，size 0；
- 用户栈 1 MiB，随机化基址；
- argv/envp 写入栈；
- 切换页表并替换 `task->mm`。

不支持按需加载 ELF 段或按需栈；匿名 mmap 是唯一 lazy 用户映射。

## 7. 页缓存

### 7.1 结构

```c
struct cached_page {
    refcnt_t refcnt;
    struct page *page;
    struct page_cache *mapping;
    pgoff_t index;
    unsigned int flags;
    spinlock_t lock;
    sleeplock_t io_lock;
    struct hlist_node ht_node;
    struct list_head dirty_list;
    struct list_head lru_list;
};

struct page_cache {
    void *host;
    const struct page_cache_ops *ops;
    refcnt_t refcnt;
    spinlock_t lock;
    struct hlist_head pages[64];
    struct list_head dirty_pages;
    unsigned long nrpages;
};
```

标志：

- `PCP_UPTODATE`
- `PCP_DIRTY`
- `PCP_ERROR`
- `PCP_WRITEBACK`

### 7.2 读入与引用

`read_mapping_page(mapping, index)`：

1. `find_or_create_page()`；
2. 已 uptodate 直接返回；
3. 获取 `io_lock`，再次检查；
4. 调用 `mapping->ops->read_page()`；
5. 成功标记 uptodate；
6. 失败标记 error，释放调用者引用。

页缓存页通常持有：

- 缓存自身的隐式引用（在 hash 中）；
- 调用者/映射引用。

### 7.3 脏页与回写

`cached_page_mark_dirty()`：

- mapping lock → page lock；
- 只在 clean→dirty 时挂入 `mapping->dirty_pages`。

`page_cache_flush_range()`：

- 收集范围内的脏页并增加引用；
- 逐页调用 `ops->write_page()`；
- 失败页重新标脏。

`writeback_single_inode()`：

1. 刷新页缓存；
2. 写 inode 元数据。

`sync_filesystem()`：

- 快照 dirty inode；
- 逐 inode 回写；
- 最后调用文件系统 `sync_fs()`。

`writeback_thread` 每 5 秒执行一次 `sync_all_filesystems()`。

### 7.4 回收

`page_cache_shrink()`：

- 从全局 LRU 最旧页开始；
- 只回收 clean、非 writeback、在 hash 中且引用计数恰好为“缓存引用 + shrink pin”的页；
- 不回收脏页，也不负责回写。

## 8. 初始化顺序

```text
head.S 早期页表
  │
  ▼
paging_init
  ├─ phys_mem_init（memblock 收集）
  ├─ final_pgtable_init
  ├─ load_final_page_table
  ├─ dt_set_fdt(phys_to_virt)
  └─ vmemmap_init
  │
  ▼
mm_core_init
  ├─ page_alloc_init
  ├─ memblock_free_all
  ├─ kmalloc_init
  └─ vmalloc_init
  │
  ▼
core_initcalls
  ├─ uvm_space_cache_init
  ├─ page_cache_init
  └─ 其他缓存
  │
  ▼
task_scheduler
  └─ 内核栈、用户页表、uvm_space 可用
```

## 9. 锁与引用规则

- `areas_lock`：伙伴分配器全局锁；
- `pcp_cache.lock`：每 CPU order-0 缓存；
- `slab_allocator.lock`：每个 slab cache；
- `vma_lock`：vmalloc 区域链表；
- `kernel_pgdir_lock`：内核页表修改与用户页表复制；
- `mapping->lock` → `cached_page->lock`：页缓存标志与 dirty 链表；
- `cached_page->io_lock`：串行化读入/回写；
- `inode->rwsem`：文件 I/O、truncate、eviction；
- `uvm_space`/`uvm_region` 由任务生命周期和 `uvm_space` 引用计数保护；
- `page` 的 `refcnt` 只用于 COW/共享页，页缓存使用自己的引用计数。

## 10. 当前限制

- brk 增长的新页未清零；
- brk 缩小不释放页；
- 用户栈只支持单页 order-8 连续分配；
- 没有 rbtree/interval tree，mmap 冲突查找是线性扫描；
- 内核页表新增映射不会同步到已创建用户页表；
- 页缓存只回收 clean page，脏页依赖 writeback；
- `uvm_space_free_stack()` 不释放 stack 的 `pages` 数组；
- fork 时 heap size 为 0 的任务不会正确复制 heap 地址；
- 没有 swap、NUMA、memory cgroup、页迁移。
