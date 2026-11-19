# mchain Physical Memory Allocator Design Document

## 1. Overview

**mchain** (memory chain) is a physical memory allocator designed for the early boot stage of the kernel. It borrows the "available memory + reserved regions" semantic model of Linux memblock, but its underlying implementation is an **array-based ordered doubly linked list**: all region entries are stored in a fixed-size static array, and each entry points to adjacent regions via signed indices `next` / `prev`; the list is sorted in ascending physical address order. Entries carry state bits and attribute bits, and merging is allowed only when the state, attributes, and NUMA node are all identical and the addresses are contiguous; `ALLOCATED` entries never participate in merging, so the exact boundary of each allocation is preserved.

**Design goals**:

- Can be initialized and operated without depending on the page allocator, slab, or any dynamic memory mechanism.
- Supports non-contiguous physical memory regions, reservation, allocation, freeing, alignment, and address range constraints.
- Provides a range iteration interface that can be consumed by page allocator initialization.
- The data structure is auditable and dumpable, facilitating bring-up debugging during early boot.

**Non-goals**: high-performance runtime allocation (should be handed over to the buddy system after boot), full support for memory hotplug, complex NUMA distance policies.

---

## 2. Terminology

| Term           | Meaning                                                                                                         |
| -------------- | --------------------------------------------------------------------------------------------------------------- |
| Region         | A contiguous physical address interval `[base, base+size)`                                                      |
| Entry          | An element in the array that describes a region and holds linked-list links                                     |
| Arena          | The array space that stores entries; initially a static array, expandable                                       |
| State bits     | The region is currently in one of FREE / RESERVED / ALLOCATED                                                   |
| Attribute bits | Memory attributes of the region, e.g. HOTPLUG / NOMAP / MIRROR / DRIVER                                         |
| Merge          | Two adjacent entries are merged into one when state, attributes, and nid are all identical (except `ALLOCATED`) |

---

## 3. Data Structures

```c
/* State bits (mutually exclusive, low 4 bits) */
#define MCHAIN_STATE_FREE        0x0u
#define MCHAIN_STATE_RESERVED    0x1u
#define MCHAIN_STATE_ALLOCATED   0x2u
#define MCHAIN_STATE_MASK        0xfu

/* Attribute bits (combinable, high bits) */
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
    phys_addr_t  base;      /* Starting physical address of the region */
    phys_size_t  size;      /* Region size, in bytes */
    int          next;      /* Index of the next region in the arena; -1 means end */
    int          prev;      /* Index of the previous region in the arena; -1 means beginning */
    int          nid;       /* NUMA node ID; MCHAIN_NID_ANY means irrelevant */
    unsigned int flags;     /* State bits | attribute bits */
    unsigned int name_id;   /* Named reservation ID; MCHAIN_NAME_NONE means unnamed */
};

struct mchain_pool {
    struct mchain_entry *entries;   /* Current arena base address */
    unsigned int capacity;  /* Arena entry capacity */
    unsigned int used;      /* Number of occupied entries */
    int          head;      /* Head index of the ordered region list; -1 means empty */
    int          free_head; /* Head of the free entry list; -1 means not yet established */
    bool         bottom_up; /* true: bottom-up; false: top-down */
    int          can_grow;  /* Whether growth is allowed */
    int          frozen;    /* Whether frozen; no region modifications allowed after freezing */
    int          free_list_ready; /* Whether the static free list has been established */
    phys_addr_t  min_limit; /* Global allocation lower bound, default 0 */
    phys_addr_t  max_limit; /* Global allocation upper bound, default MCHAIN_ANY_ADDR */
    /* Debug statistics, see §10 */
    unsigned int peak_used;       /* Peak used */
    unsigned int grow_count;      /* Number of arena growths */
    unsigned int alloc_failures;  /* Number of mchain_alloc() failures */
};

/* Static arena and global pool: initialized directly at definition, no explicit init needed */
static struct mchain_entry mchain_static_entries[MCHAIN_STATIC_CAPACITY];

struct mchain_pool mchain_pool_instance = {
    .entries         = mchain_static_entries,
    .capacity        = MCHAIN_STATIC_CAPACITY,
    .used            = 0,
    .head            = -1,
    .free_head       = -1,             /* Established lazily by mchain_bootstrap() on first modification */
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

The code above assumes that `phys_addr_t` / `phys_size_t` and `<stdbool.h>` are provided by kernel headers.

**Design points**:

- The `typedef struct { ... } mchain_*;` form is dropped; all types carry the `struct` keyword and use the full `mchain_` prefix: `struct mchain_entry`, `struct mchain_pool`, `struct mchain_iter`, `struct mchain_stats`.
- The global pool is initialized directly at definition, and like memblock, no explicit `mchain_init()` is required. Static fields (limits, direction, statistics) are set by the initializer; the free entry list cannot be conveniently listed in a C static initializer, so it is established lazily by the internal function `mchain_bootstrap(&mchain_pool_instance)` on first need, with `free_list_ready` set to 1.
- `next`, `prev`, and `free_head` all use `int`, with `-1` as the termination sentinel, avoiding extra boolean fields.
- The region list is a doubly linked list; `prev` allows top-down allocation to scan backward directly from the tail without a temporary candidate array. All insertion, removal, and splitting operations must maintain both `next` and `prev`.
- `entries` is a pointer rather than an array, so the static arena and the expanded arena share the same operation code.
- Free entries are managed through an independent `free_head` singly linked list, fully decoupled from the region list. Entries are pushed onto the free list when recycled and popped from the head when allocated.
- `bottom_up` is the global allocation direction, default `true`; it is changed via `mchain_set_bottom_up()`, and `mchain_alloc()` no longer takes a `direction` parameter.
- `can_grow` is set to 0 during expansion to prevent the expansion path from recursively triggering expansion. The initial value is 0, and it can only be enabled via `mchain_allow_grow()` after direct mapping is available.
- `frozen` is set to 1 by `mchain_freeze()` before handing over to the page allocator; afterward, all region modification APIs and `mchain_alloc()` refuse service, preventing memory already handed to the buddy system from being allocated again by mchain.
- All public APIs operate on the global `mchain_pool_instance`; internal helper functions that need multiple instances take a `struct mchain_pool *` parameter, facilitating unit testing.

---

## 4. List Maintenance

### 4.1 Finding the Insertion Position

Traverse the doubly linked list in ascending `base` order, maintaining the predecessor index `prv` of the insertion point and the current index `cur`. The insertion position satisfies: `prv == -1 || entries[prv].base + entries[prv].size <= new->base` and `cur == -1 || new->base + new->size <= entries[cur].base`. Insertion of overlapping regions is a caller error and triggers an assertion in debug builds. In production builds, the internal insertion function should still return `-EINVAL`, and the upper layer guarantees it does not happen.

### 4.2 Merge Rules

Two entries can be merged if and only if:

```
state identical && state != ALLOCATED && attr identical && nid identical && addresses contiguous
```

If named reservations are enabled (§9), `name_id` must also be identical. Addresses are contiguous when `a.base + a.size == b.base`. After merging, `base` takes the smaller value, `size` is added, the absorbed entry's `next` / `prev` are skipped, and it is returned to the free list.

**Cases where merging does not happen**:

- `ALLOCATED` entries are neither merged with `FREE` / `RESERVED`, nor with adjacent `ALLOCATED` entries. This is a hard rule: each allocation must retain an independent entry; otherwise two adjacent allocations would be merged into one block, and `mchain_free()` could not locate and free one of them using the original `(base, size)`.
- No merging across states: `RESERVED` and `FREE` remain independent even if addresses are contiguous; reserved regions are always visible.
- Adjacent regions with different attributes or `nid` are not merged, because their mapping semantics or NUMA ownership differ.
- Named reservations with different `name_id` are not merged, avoiding mixing different reservations when looking up by name.

`RESERVED` and `RESERVED` are merged when their keys are identical, so a reserved area may become a larger entry; releasing a reservation must use `mchain_unreserve()`, which supports partial ranges, and cannot rely on entry boundaries.

### 4.3 Insertion (`mchain_insert_entry`)

1. The caller must first pre-allocate the required entries via `mchain_ensure_entries()` (see §4.5 and §7). `mchain_insert_entry()` itself does not trigger expansion, and once the insertion phase is entered, it will not fail due to insufficient entries.
2. Fill in `base`, `size`, `state | attrs`, `nid`; if named reservations are enabled, also fill in `name_id` (`MCHAIN_NAME_NONE` for ordinary regions).
3. Locate the insertion position and check mergeability with the predecessor and successor; `ALLOCATED` entries skip the merge check and are inserted directly:
   - Only the predecessor is mergeable: extend the predecessor's `size`; if the predecessor's new tail is also mergeable with the successor, absorb the successor. The new entry itself is unused and must be returned to the free list.
   - Only the successor is mergeable: the new entry extends to cover the successor, the successor entry is recycled, and the new entry takes over its successor pointer and fixes `prev`.
   - Both predecessor and successor are mergeable: extend the predecessor to cover everything and recycle the successor entry; the new entry itself is unused and must be returned to the free list.
   - Neither is mergeable: insert the new entry between `prv` and `cur`, and fix the `prev` / `next` on both sides.
4. Before returning absorbed entries or the unused new entry to the free list, fully unlink them from the `next` / `prev` chain, then push them onto the free list.

### 4.4 Carving (`mchain_carve_range`)

Carve out bytes from `[start, end)` that satisfy the predicate `(state, attr_mask, nid)` from the region list, used for reservation, allocation, and unreservation. **It does not unconditionally carve out the entire range**: non-matching entries must be preserved as-is; otherwise reservation or allocation would mistakenly delete memory in other states.

Traverse the list and classify each matching and intersecting entry:

- **Fully covered** (`start <= base && base + size <= end`): remove from the list, return the entry to the free list.
- **Covers the left half** (`start <= base < end < base+size`): adjust `base = end`, `size = old_base + old_size - end`. Note that it cannot be written as `size -= (end - start)`, because `start` may be less than `base`.
- **Covers the right half** (`base < start < base+size <= end`): `size = start - base`.
- **Covers the middle** (`base < start && end < base+size`): the original entry retains the left fragment (`size = start - base`), and a new entry carries the right fragment (`base = end`, `size = old_end - end`, with state/attributes/nid/name_id copied); the new entry's `next` points to the original entry's old `next`, the original entry's old successor's `prev` points to the new entry, then set the original entry's `next` to the new entry and the new entry's `prev` to the original entry. **The order must not be reversed**, otherwise the successor chain is lost.

Carving only modifies the list structure and entry ownership, not the flag bits of the remaining fragments; the caller decides in which state the carved-out range is re-inserted. Middle splitting requires additional entries, and the caller must pre-allocate them in advance; otherwise `mchain_carve_range()` returns `-ENOMEM` and must not leave partial modifications.

### 4.5 Atomicity of Modifications

All public modification APIs that need to add entries follow the order "count → pre-allocate → modify":

1. Read-only scan to compute the number of new entries required by this operation. A single split requires at most 2 extra entries (among the left fragment, right fragment, and carved-out fragment, the original entry can be reused to carry one of them); if the candidate exactly covers the entire free entry, the original entry can be changed in place without extra entries; covering one end of an entry requires only 1. The implementation should compute precisely based on the actual position, rather than always pre-allocating 2, otherwise when the arena has only a few slots left, expansion may be triggered unnecessarily or even fail. Note that the entire entry must not be carved out first and then unconditionally inserted, otherwise "covering the whole segment" would also incorrectly require an extra entry.
2. Call `mchain_ensure_entries(pool, nr, avoid_start, avoid_end)`. If free entries are insufficient and growth is allowed, `mchain_grow_pool()` is triggered; if still insufficient, return `-ENOMEM`, and the target `avoid` range has not been modified (though some expansion may already have completed and consumed other free memory). The `avoid` interval represents the target range that the current operation must preserve; expansion must not allocate a new arena from this interval, otherwise the semantics of reservation, unreservation, or allocation would be changed by the expansion itself.
3. Perform carving and insertion. Since entries have been pre-allocated, this phase must not fail due to insufficient entries.

Expansion itself consumes a piece of free memory as the new arena and registers this interval as `ALLOCATED`; this is an expected side effect of expansion and does not belong to the atomic scope of the target operation. Therefore "expansion succeeded but subsequent allocation failed" leaves a larger arena and the memory it occupies, but does not leave partial modifications to the target allocation, reservation, or unreservation. `mchain_ensure_entries()` must ensure that the expansion process does not recursively call public APIs that require pre-allocation.

---

## 5. Public API Overview

The complete function signatures, parameter constraints, return values, and error codes are in §14; this section only explains the core semantics. All APIs return error codes or special values on failure and do not trigger panic (except for debug assertions).

### 5.1 Static Initialization, Allocation Direction, and Lifecycle

```c
int  mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
void mchain_set_bottom_up(bool bottom_up);
bool mchain_bottom_up(void);
void mchain_allow_grow(void);
void mchain_freeze(void);
```

The global pool `mchain_pool_instance` is statically initialized directly at definition: empty list, limits `[0, MCHAIN_ANY_ADDR)`, `bottom_up = true`, `can_grow = 0`, `frozen = 0`. Like memblock, **no explicit `mchain_init()` is required**. The free entry list cannot be conveniently listed with a normal C static initializer, so it is established lazily by the internal function `mchain_bootstrap()` in the first modification API that needs the free list; before that, queries and iterators see an empty list.

`mchain_set_limit()` can adjust the global allocation bounds during boot, e.g. after the direct mapping range is expanded. It only affects subsequent allocations and does not modify existing regions; `max_addr == MCHAIN_ANY_ADDR` means no upper bound. Returns `-EINVAL` for invalid parameters, and the original settings remain unchanged.

`mchain_set_bottom_up()` sets the global allocation direction: `true` for bottom-up, `false` for top-down, default `true`. It references memblock's `memblock_set_bottom_up()` and only affects subsequent `mchain_find_in_range()` / `mchain_alloc()`, without changing existing regions. `mchain_bottom_up()` returns the current direction.

`mchain_allow_grow()` is called after the direct mapping is established to allow the arena to grow on demand. Before that, the static arena must be large enough; the expansion path depends on `phys_to_virt()` to access the new arena.

`mchain_freeze()` is called before handing free memory over to the buddy system. After freezing, `mchain_alloc()` and `mchain_find_in_range()` (used for planning allocations) return 0, `mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` / `mchain_free()` return `-EBUSY`, and expansion is disabled; size/state queries, dump, and iterators remain available to complete the handover. Freezing is irreversible (unless the global pool is re-initialized, test scenarios only).

### 5.2 Adding, Reserving, and Unreserving

```c
int mchain_add(phys_addr_t base, phys_size_t size,
               unsigned int attrs, int nid);
int mchain_reserve(phys_addr_t base, phys_size_t size,
                   unsigned int attrs, int nid);
int mchain_unreserve(phys_addr_t base, phys_size_t size);
```

`mchain_add()` only adds `FREE` regions. When the target range overlaps existing entries, the bytes covered by existing entries **remain unchanged**, and only the uncovered bytes are inserted as `FREE | attrs`; the newly inserted gaps can merge with adjacent same-key `FREE` entries. This is consistent with memblock's `memblock_add()` semantics. Do not use `mchain_add()` to add reserved or allocated regions.

`mchain_reserve()` only acts on `FREE` bytes within the target range: after carving out these bytes, they are inserted as `MCHAIN_STATE_RESERVED | (source attrs | attrs)`. Preserving the source free region's attributes avoids losing physical memory attributes such as NOMAP / HOTPLUG / MIRROR / DRIVER due to a state change; `attrs` can only add attributes, not clear source attributes. Bytes in the target range that are already `RESERVED` or `ALLOCATED` remain unchanged, so partial overlap is allowed and will not overwrite existing allocations. To strictly confirm whether the entire range can be reserved, first call `mchain_range_is_free()`. Only bytes already registered as `FREE` by `mchain_add()` can be reserved; if the target range has not been added yet (lies in a hole), that part will not be recorded as reserved, so the boot flow must **add all available memory first, then perform reservations**.

`mchain_unreserve()` is the inverse of `mchain_reserve()`: it only converts `RESERVED` bytes within the target range back to `FREE`, supporting partial release; `FREE` and `ALLOCATED` bytes are unaffected.

### 5.3 Finding, Allocating, and Freeing

```c
phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
                                 phys_addr_t min_addr, phys_addr_t max_addr,
                                 int nid, unsigned int attr_mask);
phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
                         phys_addr_t min_addr, phys_addr_t max_addr,
                         int nid, unsigned int attr_mask);
int mchain_free(phys_addr_t base, phys_size_t size);
```

`mchain_find_in_range()` only searches and does not modify the list; `mchain_alloc()` carves out and inserts an `ALLOCATED` entry using the same search rules.

- When `min_addr` / `max_addr` are `MCHAIN_ANY_ADDR`, they are replaced by the global `min_limit` / `max_limit` respectively; after replacement, they are intersected with the global limits. `max_addr` is an exclusive upper bound.
- `nid == MCHAIN_NID_ANY` means any node is acceptable; when a specific node is given, an entry whose `nid` equals that node or is `MCHAIN_NID_ANY` both match.
- `attr_mask` is the set of attribute bits that must match, using only bits in `MCHAIN_ATTR_MASK`. `MCHAIN_ATTR_NOMAP` and `MCHAIN_ATTR_DRIVER` are skipped by default and only participate in allocation when explicitly required in `attr_mask`, preventing ordinary allocations from mistakenly taking memory that is not directly mappable or is driver-managed.
- The allocation direction is determined by the global `mchain_set_bottom_up()`, default bottom-up; it is no longer passed as a parameter.
- The candidate interval must fall entirely within the effective range after applying the `MCHAIN_ANY_ADDR` rules and satisfy alignment; `align` must be a power of 2.
- On success, only the exact `[candidate, candidate + size)` is allocated, **without absorbing alignment fragments**, so `mchain_free(base, size)` can precisely match using the original size saved by the caller. The allocated entry inherits all attributes and `nid` of the source free entry.
- Returns `0` on failure; physical address `0` is reserved as the failure sentinel and will not be allocated.

`mchain_free()` looks up an exactly matching `ALLOCATED` entry by `(base, size)`, converts it to `FREE`, and merges it with adjacent free entries. Partial freeing is not supported; to release a reserved area, use `mchain_unreserve()`.

### 5.4 Queries, Iteration, and Debugging

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

Query functions are read-only and do not trigger expansion. The iterator only returns `FREE` intervals, skipping entries with non-matching `nid` / attributes and entries with no intersection with the address limits; the list must not be modified during iteration. The optional named reservation APIs `mchain_reserve_named()` / `mchain_find_reserved()` are described in §9.

---

## 6. Allocation Path Details

### 6.1 Alignment and Fragment Handling

The first version **does not merge alignment fragments into the allocated region**. `mchain_alloc()` always allocates only the exact `[candidate, candidate + size)`, so `mchain_free(base, size)` can directly match the `ALLOCATED` entry using the original size saved by the caller. If the small fragments on the left or right were merged into the allocated entry, the actual range of the allocated entry would be larger than the requested value, and the caller would not know the actual size, so freeing would inevitably mismatch.

Therefore `MCHAIN_MIN_FRAG` is not enabled in the first version and is reserved only as a future optimization. If fragments are to be merged in the future, an output of the actual allocated size must be provided at the same time (e.g. adding `mchain_alloc_ex(..., phys_size_t *allocated_size)`), and the caller must pass the actual size to `mchain_free()`; the allocation path cannot be changed without changing the free contract.

### 6.2 Order of Address Limit Checks

First replace `min_addr` / `max_addr` with the global limits according to the `MCHAIN_ANY_ADDR` rules, then compute the effective range `[lo, hi) = [max(min_addr, pool.min_limit), min(max_addr, pool.max_limit))`, then filter entries by `lo` / `hi` (skip entries whose interval has no intersection with the effective range), and finally compute the candidate start within the entry and perform a secondary check. This avoids performing alignment calculations on entries that cannot possibly satisfy the request, and ensures the global limits cannot be bypassed by parameters.

### 6.3 Reverse Traversal

The first version uses a **doubly linked list**: `struct mchain_entry` already contains the `prev` index. When the global direction is top-down (`mchain_bottom_up() == false`), allocation starts from the tail of the list and scans backward along `prev`, with the same time complexity as bottom-up. All insertion, removal, merging, and middle splitting operations must maintain both `next` and `prev`, using the `-1` sentinel symmetrically.

The "forward traversal collecting candidate indices into a temporary array" approach is not used: that approach requires an additional capacity limit, rollback logic, and is more error-prone when the arena grows or the list changes. The extra `int` field of the doubly linked list is far below the complexity cost of a temporary array.

---

## 7. Dynamic Expansion

### 7.1 Trigger Conditions and Prerequisites

All operations that may add entries call `mchain_ensure_entries(pool, nr, avoid_start, avoid_end)` to pre-allocate entries before modifying the list; when free entries are insufficient and `can_grow == 1`, `mchain_ensure_entries()` triggers `mchain_grow_pool()`. `mchain_insert_entry()` itself does not trigger expansion, so once the modification phase is entered, it will not fail due to lack of entries.

The `avoid` interval is the target range that the current operation must preserve. Expansion needs to allocate a new arena from free memory; if this memory happens to fall within the range currently being reserved, unreserved, or allocated, the operation semantics would be changed by the expansion itself. Therefore expansion must look for space in the two segments `[0, avoid_start)` and `[avoid_end, max_limit)`, and must not intersect `avoid`; `avoid_end == avoid_start` means no restriction.

`can_grow` is initially 0. Only when the direct mapping has been established and `phys_to_virt()` can access the new arena does calling `mchain_allow_grow()` set it to 1. The critical boot path should reserve a sufficiently large `MCHAIN_STATIC_CAPACITY` so that expansion serves only as a fallback.

### 7.2 Expansion Flow (Bootstrap)

Expansion has a critical bootstrap constraint: when expansion is triggered, the old arena has no free entries at all, so expansion **cannot** be done by "first using `mchain_alloc()` to insert an ALLOCATED entry to register the new arena"—that path itself requires free entries. The correct order is as follows:

1. Set `can_grow = 0`, compute `new_capacity = old_capacity * MCHAIN_GROW_FACTOR + MCHAIN_GROW_HEADROOM` (default `2 * old + 2`), and check for multiplication and addition overflow.
2. Compute `new_bytes = align_up(new_capacity * sizeof(struct mchain_entry), MCHAIN_ARENA_ALIGN)`, check that multiplication, addition, and `align_up` do not wrap; then use the read-only `mchain_find_in_range()` (or an equivalent internal lookup function) to find a free physical interval of `new_bytes` bytes. The search must avoid the current operation's `avoid` interval: first try `[0, avoid_start)`, and if that fails, try `[avoid_end, max_limit)`; when `avoid_end == avoid_start`, perform only one full-range search. The arena must remain stable for the kernel's lifetime, so `MCHAIN_ATTR_HOTPLUG` and `MCHAIN_ATTR_DRIVER` regions should be skipped. This step does not modify the list.
3. Obtain the virtual address of the new arena via `phys_to_virt()`, and **compactly copy** the valid entries from the old arena to the new arena in region-list order, rebuilding each entry's `next` / `prev`; rebuild `free_head` for the remaining slots. Compact copying renumbers in list order and does not require a mapping table from old indices to new indices.
4. Switch `entries` and `capacity` to the new arena. At this point the new arena already has free slots.
5. Use a slot in the new arena to register the physical interval found in step 2 as `ALLOCATED` (splitting the original `FREE` entry if necessary), inheriting the source free entry's `attrs` / `nid`. `MCHAIN_GROW_HEADROOM` ensures that this step and the caller's subsequent insertions have enough slots.
6. If the old arena was dynamically allocated (`old_entries != mchain_static_entries`), return the old arena via `mchain_free()`. **The static arena resides in the kernel image/BSS and must not be freed**, otherwise kernel data would be treated as free memory.
7. Set `can_grow = 1`, `grow_count++`, and return 0.

Step 5 must not call the public `mchain_alloc()` when registering the arena itself, otherwise it would recursively enter expansion; instead, it should use the internal "carve free entry + insert ALLOCATED" path, with `can_grow == 0` ensuring no re-entry.

### 7.3 Failure Handling

- Step 2 cannot find a sufficiently large free interval: restore `can_grow = 1`, return `-ENOMEM`, and the old list remains unchanged.
- Failures after step 3 are implementation errors (e.g. insufficient `MCHAIN_GROW_HEADROOM`); debug builds should assert. Production builds should ensure these steps cannot fail through capacity headroom.
- Callers (`mchain_add()`, `mchain_reserve()`, `mchain_alloc()`, etc.) return `-ENOMEM` or `0` directly when pre-allocation fails, without modifying the list.
- After successful expansion, even if the subsequent target operation fails, the target region will not be left partially modified; however, expansion has already consumed a piece of free memory as the arena and registered it as `ALLOCATED`, and this memory will not return to the free state unless the arena is explicitly freed in the future.

---

## 8. Iterator Interface

Intended for the page allocator initialization to consume free intervals, avoiding the need for callers to understand the internal list structure.

```c
struct mchain_iter {
    int          cur;        /* Current entry index, maintained only internally by the iterator */
    phys_addr_t  min_addr;
    phys_addr_t  max_addr;
    int          nid;
    unsigned int attr_mask;
};

void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
                      phys_addr_t max_addr, int nid, unsigned int attr_mask);

int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);
```

`mchain_iter_next()` skips non-`FREE` entries, entries with non-matching `nid` / attributes, and entries with no intersection with the address limits; for matching entries, it returns the intersection sub-interval with `[min_addr, max_addr)`, returning 1 on success and 0 when iteration ends. Each entry produces at most one interval, so no `offset` field is needed.

- When `min_addr` / `max_addr` are `MCHAIN_ANY_ADDR`, they mean no lower bound / upper bound respectively; `max_addr` is an exclusive upper bound.
- Consistent with the allocation path, `MCHAIN_ATTR_NOMAP` and `MCHAIN_ATTR_DRIVER` are skipped by default, and such intervals are returned only when explicitly required in `attr_mask`.
- The iterator is read-only and does not trigger expansion; modifying the list during iteration invalidates `cur`, and the caller must complete modifications and re-initialize the iterator.
- This interface directly corresponds to memblock's `for_each_free_mem_range()` and is the basis for page allocator initialization and handover functions like `mchain_free_all()`. During handover, `mchain_freeze()` should be called first, then iterate over the free intervals to hand them to the buddy system, preventing further mchain allocations from taking memory from these intervals during or after iteration.

---

## 9. Named Reservations

If kexec or similar scenarios need to look up reserved regions by name, strings cannot be stored directly on `struct mchain_entry` (entries are fixed-size). The current implementation uses the following scheme:

- Add `unsigned int name_id`, pointing to a static string table; `MCHAIN_NAME_NONE` (0) means unnamed. `mchain_reserve_named()` receives `name`, looks it up or registers it in the table, and then writes `name_id`.
- The lookup interface traverses the list matching `name_id` and returns the first matching reserved interval.

`name_id` **must participate in the merge condition**, otherwise adjacent "named reservation" and "unnamed reservation" would be merged and the name would be lost with the entry; two adjacent reservations with different names would also be incorrectly merged. The merge key becomes:

```
state identical && state != ALLOCATED && attr identical && nid identical &&
name_id identical && addresses contiguous
```

Thus only adjacent reservations with the same name, attributes, and node are merged. Names belong only to the `RESERVED` state: `FREE` and `ALLOCATED` entries must have `name_id` equal to `MCHAIN_NAME_NONE`, and free fragments released by `mchain_unreserve()` must not inherit the name, otherwise free memory merging would be hindered. If the same name corresponds to multiple non-contiguous intervals, `mchain_find_reserved()` returns the first match; to enumerate all, the iterator can be extended to accept a state filter, or the region list can be traversed directly.

This implementation has placed `name_id` in `struct mchain_entry` and implemented `mchain_reserve_named()` / `mchain_find_reserved()`. If named reservations are not needed, the `name_id` of all ordinary regions remains `MCHAIN_NAME_NONE`, which does not affect other paths.

---

## 10. Debugging and Statistics

- **`mchain_dump()`**: prints `[base, base+size)`, state name, attribute names, `nid`, `name_id`, entry index, and `prev` / `next` pointers in list order, facilitating detection of list breakage or incorrect merging. After expansion, it should also print the arena base address, capacity, `used`, and `free_head`.
- **`mchain_validate()`**: first checks the preconditions when not bootstrapped (`head` / `free_head` are -1, `used` is 0), then ensures `mchain_bootstrap()` has been executed, then checks all invariants listed in §14.12, returning 0 on success and `-EINVAL` on failure. It can be called with `assert(mchain_validate() == 0)` after each modification (debug builds only).
- **`mchain_get_stats()`**: returns `used` / `peak_used`, capacity, number of expansions, number of allocation failures, frozen flag, and the byte counts of `FREE` / `RESERVED` / `ALLOCATED`; output a summary once in the late boot stage to tune `MCHAIN_STATIC_CAPACITY`.
- **Assertions**: check for overlap at the entrance of `mchain_insert_entry`; check list ordering and `prev` / `next` symmetry after carving and splitting; check index validity after expansion migration. Enabled in debug builds, compiled out in production builds.

---

## 11. Configuration Constants and Sizing Recommendations

| Constant                 | Suggested Value   | Description                                                                                                                         |
| ------------------------ | ----------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `MCHAIN_STATIC_CAPACITY` | 256 ~ 512         | Estimate based on worst case: firmware mapping entries × 2 + kernel reservations × 2 + split headroom                               |
| `MCHAIN_GROW_FACTOR`     | 2                 | Capacity doubles with each expansion                                                                                                |
| `MCHAIN_GROW_HEADROOM`   | 2                 | Extra slots after expansion, for registering the new arena itself and subsequent splits                                             |
| `MCHAIN_ARENA_ALIGN`     | `PAGE_SIZE`       | Physical alignment of the new arena; also facilitates `phys_to_virt()` access                                                       |
| `MCHAIN_MIN_FRAG`        | 0 (first version) | The first version does not merge fragments; enabling it requires providing actual allocation size output at the same time, see §6.1 |

---

## 12. Implementation Order

1. **Core doubly linked list and merging/carving**: statically initialized `mchain_pool_instance`, `mchain_bootstrap`, `mchain_insert_entry`, `mchain_carve_range`, with unit tests covering adjacent merging, `ALLOCATED` not merging, overlap handling, middle splitting, different attributes not merging, and `prev` / `next` symmetry.
2. **Pre-allocation and atomicity**: `mchain_ensure_entries`, free list of the static arena, pre-modification counting and pre-allocation, testing "failure of the target region leaves no partial modifications" (allowing expansion to have completed and consumed memory).
3. **Add, reserve, unreserve**: `mchain_add`, `mchain_reserve`, `mchain_unreserve`, covering partial overlap and partial release.
4. **Allocation and freeing**: `mchain_find_in_range`, `mchain_alloc` (first only supporting bottom-up and no address limits), `mchain_free`, confirming exact allocation size and independent `ALLOCATED` entries.
5. **Iterator**: `mchain_iter_*`, and use it to verify "sum of free intervals = available memory − reserved − allocated".
6. **Address limits and top-down**: implement reverse scanning using the existing `prev` index, select top-down via `mchain_set_bottom_up(false)`, and add `min_addr` / `max_addr` filtering and `MCHAIN_ANY_ADDR`.
7. **flags splitting and nid**: attribute bits and node IDs enter merge conditions and matching rules, with regression testing of all merge scenarios.
8. **Dynamic expansion**: implement according to the bootstrap order in §7.2, with emphasis on testing that the static arena is not freed, the dynamic arena is freed, and `MCHAIN_GROW_HEADROOM` is sufficient.
9. **Debugging and statistics**: `mchain_validate`, `mchain_dump`, `mchain_get_stats` can be interspersed at any stage, the earlier the better.
10. **Handover and freezing**: implement `mchain_freeze()`, freeze the allocator before handing over to the page allocator, then use the iterator to hand all `FREE` intervals to the buddy system.

---

## 13. Semantic Comparison with memblock

| memblock                     | mchain                                                               |
| ---------------------------- | -------------------------------------------------------------------- |
| `memblock_add`               | `mchain_add(..., attrs, nid)` (adds FREE only)                       |
| `memblock_reserve`           | `mchain_reserve`                                                     |
| `memblock_remove`            | No direct equivalent; use `mchain_unreserve` to release reservations |
| `memblock_phys_free`         | `mchain_free` / `mchain_unreserve`                                   |
| `memblock_find_in_range`     | `mchain_find_in_range`                                               |
| `memblock_phys_alloc`        | `mchain_alloc`                                                       |
| `memblock_set_current_limit` | `mchain_set_limit` + per-call parameter override                     |
| `memblock_bottom_up`         | `mchain_bottom_up`                                                   |
| `memblock_set_bottom_up`     | `mchain_set_bottom_up`                                               |
| `for_each_free_mem_range`    | `mchain_iter_next`                                                   |
| `memblock_double_array`      | `mchain_grow_pool`                                                   |
| `memblock_free_all`          | `mchain_freeze` + `mchain_iter_next` handover                        |
| `memblock_dump_all`          | `mchain_dump`                                                        |

memblock expresses "available − reserved" with two arrays, `memory` and `reserved`; mchain expresses the same semantics with `FREE` / `RESERVED` / `ALLOCATED` states on a single linked list. Therefore `mchain_add()` is only responsible for adding available memory, and reservation and allocation must go through their respective state transition APIs; the reserved/allocated states cannot be stuffed into the list via `mchain_add()`. This comparison allows developers familiar with memblock to get up to speed quickly, and also facilitates cross-validation of the two outputs during the bring-up stage.

---

## 14. Complete API Reference

> This chapter gives the final API contract of mchain. §5 is a semantic overview; if this chapter conflicts with the signatures or details of earlier chapters, this chapter prevails, and the earlier text should be revised synchronously during implementation. All symbols use the full `mchain_` / `MCHAIN_` prefix; types always retain the `struct` keyword and do not use typedefs.

### 14.1 Public Constants and Types

```c
/* Capacity and expansion */
#ifndef MCHAIN_STATIC_CAPACITY
#define MCHAIN_STATIC_CAPACITY       256
#endif
#define MCHAIN_GROW_FACTOR           2
#define MCHAIN_GROW_HEADROOM         2
#define MCHAIN_ARENA_ALIGN           PAGE_SIZE

/* State bits: mutually exclusive, in the low 4 bits */
#define MCHAIN_STATE_FREE            0x0u
#define MCHAIN_STATE_RESERVED        0x1u
#define MCHAIN_STATE_ALLOCATED       0x2u
#define MCHAIN_STATE_MASK            0xfu

/* Attribute bits: combinable, in the high bits */
#define MCHAIN_ATTR_HOTPLUG          (1u << 4)
#define MCHAIN_ATTR_NOMAP            (1u << 5)
#define MCHAIN_ATTR_MIRROR           (1u << 6)
#define MCHAIN_ATTR_DRIVER           (1u << 7)
#define MCHAIN_ATTR_MASK             0xfffffff0u

/* Special values */
#define MCHAIN_NID_ANY               (-1)
#define MCHAIN_ANY_ADDR              ((phys_addr_t)~(phys_addr_t)0)

/* Named reservations */
#define MCHAIN_NAME_SLOTS            32
#define MCHAIN_NAME_MAX              32
#define MCHAIN_NAME_NONE             0u

/* Iterator, field semantics see §14.7 */
struct mchain_iter {
    int          cur;
    phys_addr_t  min_addr;
    phys_addr_t  max_addr;
    int          nid;
    unsigned int attr_mask;
};

/* Debug statistics, see §14.8 */
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

Conventions:

- `MCHAIN_STATE_*` and `MCHAIN_ATTR_*` are stored together in `struct mchain_entry.flags`; the `attrs` parameter of public APIs accepts only bits in `MCHAIN_ATTR_MASK`, and passing state bits returns `-EINVAL`.
- `MCHAIN_ATTR_DRIVER` indicates a driver-managed region; ordinary allocation and page allocator handover should skip it by default. Only when explicitly required in `attr_mask` do the allocator and iterator return such regions.
- `MCHAIN_ANY_ADDR` as the `min_addr` of an allocation API means use `min_limit`, and as the `max_addr` means use `max_limit`; in the iterator, `MCHAIN_ANY_ADDR` means no boundary on that side.
- `MCHAIN_NID_ANY` can be used both to request "any node" and to mark "the NUMA ownership of this region is irrelevant".
- All address intervals are half-open intervals `[base, base + size)`, and all `size` values are in bytes.

### 14.2 API Overview

Summary of public API declarations:

```c
/* Lifecycle and allocation direction */
int          mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
void         mchain_set_bottom_up(bool bottom_up);
bool         mchain_bottom_up(void);
void         mchain_allow_grow(void);
void         mchain_freeze(void);

/* Region management */
int          mchain_add(phys_addr_t base, phys_size_t size,
                        unsigned int attrs, int nid);
int          mchain_reserve(phys_addr_t base, phys_size_t size,
                            unsigned int attrs, int nid);
int          mchain_unreserve(phys_addr_t base, phys_size_t size);

/* Named reservations */
int          mchain_reserve_named(const char *name, phys_addr_t base,
                                  phys_size_t size, unsigned int attrs, int nid);
int          mchain_find_reserved(const char *name, phys_addr_t *base,
                                  phys_size_t *size);

/* Find, allocate, and free */
phys_addr_t  mchain_find_in_range(phys_size_t size, phys_size_t align,
                                  phys_addr_t min_addr, phys_addr_t max_addr,
                                  int nid, unsigned int attr_mask);
phys_addr_t  mchain_alloc(phys_size_t size, phys_size_t align,
                          phys_addr_t min_addr, phys_addr_t max_addr,
                          int nid, unsigned int attr_mask);
int          mchain_free(phys_addr_t base, phys_size_t size);

/* Queries */
bool         mchain_range_is_free(phys_addr_t base, phys_size_t size);
bool         mchain_is_memory(phys_addr_t addr);
phys_size_t  mchain_free_size(void);
phys_size_t  mchain_reserved_size(void);
phys_size_t  mchain_memory_size(void);
phys_addr_t  mchain_start_of_dram(void);
phys_addr_t  mchain_end_of_dram(void);
unsigned int mchain_used_entries(void);
unsigned int mchain_capacity(void);

/* Iterator */
void         mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr,
                              phys_addr_t max_addr, int nid,
                              unsigned int attr_mask);
int          mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);

/* Debugging and statistics */
int          mchain_validate(void);
void         mchain_dump(void);
void         mchain_get_stats(struct mchain_stats *stats);
```

| Category    | API                                                                | Success Return           | Failure Return                    |
| ----------- | ------------------------------------------------------------------ | ------------------------ | --------------------------------- |
| Lifecycle   | `mchain_set_limit`                                                 | `0`                      | `-EINVAL`                         |
|             | `mchain_set_bottom_up` / `mchain_bottom_up`                        | none / current direction | none                              |
|             | `mchain_allow_grow`                                                | none                     | none                              |
|             | `mchain_freeze`                                                    | none                     | none                              |
| Region mgmt | `mchain_add`                                                       | `0`                      | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
|             | `mchain_reserve`                                                   | `0`                      | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
|             | `mchain_unreserve`                                                 | `0`                      | `-EINVAL` / `-ENOMEM` / `-EBUSY`  |
| Allocation  | `mchain_find_in_range`                                             | physical address         | `0`                               |
|             | `mchain_alloc`                                                     | physical address         | `0`                               |
|             | `mchain_free`                                                      | `0`                      | `-EINVAL` / `-ENOENT` / `-EBUSY`  |
| Queries     | `mchain_range_is_free`                                             | `true` / `false`         | none                              |
|             | `mchain_is_memory`                                                 | `true` / `false`         | none                              |
|             | `mchain_free_size` / `mchain_reserved_size` / `mchain_memory_size` | byte count               | none                              |
|             | `mchain_start_of_dram` / `mchain_end_of_dram`                      | physical address         | none                              |
|             | `mchain_used_entries` / `mchain_capacity`                          | entry count              | none                              |
| Iteration   | `mchain_iter_init`                                                 | none                     | none                              |
|             | `mchain_iter_next`                                                 | `1`                      | `0` (iteration finished)          |
| Debugging   | `mchain_validate`                                                  | `0`                      | `-EINVAL`                         |
|             | `mchain_dump` / `mchain_get_stats`                                 | none                     | none                              |
| Named res.  | `mchain_reserve_named`                                             | `0`                      | `-EINVAL` / `-ENOMEM` / `-ENOSPC` |
|             | `mchain_find_reserved`                                             | `0`                      | `-ENOENT` / `-EINVAL`             |

### 14.3 Static Initialization, Allocation Direction, and Lifecycle

#### Static Initialization and `mchain_bootstrap`

The global pool is initialized directly at definition; `mchain_init()` is not needed:

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

- `head = -1`: the region list is empty.
- `free_head = -1`, `free_list_ready = 0`: the static free list has not been established.
- `bottom_up = true`: default bottom-up.
- `can_grow = 0`, `frozen = 0`: expansion disabled, not frozen.
- Default allocation range `[0, MCHAIN_ANY_ADDR)`; can be adjusted via `mchain_set_limit()`.
- **No memory regions are added**. The caller subsequently uses `mchain_add()` to add the available memory reported by firmware, then `mchain_reserve()` to reserve firmware-occupied areas and the kernel image.

`mchain_bootstrap(struct mchain_pool *pool)` is an internal function, not a public API. The first public API that needs the free list (`mchain_add()`, `mchain_reserve()`, `mchain_unreserve()`, `mchain_alloc()`, `mchain_free()`, and `mchain_validate()` / `mchain_dump()`) calls `mchain_bootstrap(&mchain_pool_instance)`: it strings all static entries into the free list, sets `free_head = capacity ? 0 : -1`, `free_list_ready = 1`. The function is idempotent, does not depend on the page allocator or locks, and only traverses the static array.

Before bootstrap, read-only size/state queries and `mchain_iter_*` see an empty list; `mchain_set_limit()`, `mchain_set_bottom_up()`, `mchain_allow_grow()`, `mchain_freeze()` can be called before then. To restore the initial state in tests, the global `mchain_pool_instance`, the static arena, and the name table must be re-initialized; production code should not re-initialize.

#### `mchain_set_limit`

```c
int mchain_set_limit(phys_addr_t min_addr, phys_addr_t max_addr);
```

Adjusts the global allocation bounds; only affects subsequent `mchain_find_in_range()` / `mchain_alloc()`, without modifying any region entries.

- `min_addr` is an inclusive lower bound, `max_addr` is an exclusive upper bound.
- `max_addr == MCHAIN_ANY_ADDR` means no upper bound; `min_addr == MCHAIN_ANY_ADDR` is treated as 0.
- `min_addr >= max_addr` returns `-EINVAL`, and the original settings remain unchanged.
- This API is used in scenarios where the addressable range expands or shrinks during boot; the default limit is `[0, MCHAIN_ANY_ADDR)`.

#### `mchain_set_bottom_up` / `mchain_bottom_up`

```c
void mchain_set_bottom_up(bool bottom_up);
bool mchain_bottom_up(void);
```

- `mchain_set_bottom_up(true)` selects bottom-up: `mchain_find_in_range()` / `mchain_alloc()` scan from the head of the list to the tail.
- `mchain_set_bottom_up(false)` selects top-down: scan from the tail along `prev` toward the head.
- The default value is `true`, consistent with memblock.
- The setting only affects subsequent lookups and allocations, without changing existing regions; it can be called before any allocation.
- `mchain_bottom_up()` returns the current direction.
- References memblock's `memblock_set_bottom_up()` / `memblock_bottom_up()`; `mchain_alloc()` no longer takes a `direction` parameter.

#### `mchain_allow_grow`

```c
void mchain_allow_grow(void);
```

Enables automatic arena expansion. It can only be called after the following conditions are met:

- The final direct mapping has been established, and `phys_to_virt()` can access any newly allocated arena.
- The kernel image and the region containing the static arena have been reserved and will not be taken by subsequent allocations.

Before the first call, `can_grow == 0`; when static entries are exhausted, `mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` return `-ENOMEM`, and `mchain_alloc()` returns `0`. The function is idempotent and can be called multiple times during the boot flow.

#### `mchain_freeze`

```c
void mchain_freeze(void);
```

Freezes the allocator, used before handing over to the page allocator. The function is idempotent.

- Sets `frozen = 1`, `can_grow = 0`.
- `mchain_alloc()` / `mchain_find_in_range()` return 0.
- `mchain_add()` / `mchain_reserve()` / `mchain_unreserve()` / `mchain_free()` return `-EBUSY`.
- `mchain_allow_grow()` is a no-op after freezing.
- Queries, `mchain_iter_*`, `mchain_dump()` / `mchain_validate()` / `mchain_get_stats()` remain available to complete the handover and post-hoc auditing.
- Freezing is irreversible; to reuse the allocator, the global pool can only be re-initialized (test scenarios only).
- The handover order should be: first `mchain_freeze()`, then iterate over `FREE` intervals to hand them to the buddy system; thus no new mchain allocation will take memory from the same intervals during iteration.
- After freezing, `FREE` entries remain in the list for the handover iterator; after the handover is complete, these intervals belong to the buddy system, and query results are only for auditing and cannot be used for mchain allocation.
- `RESERVED` intervals that need to be handed to the buddy system must first be `mchain_unreserve()`d, and `ALLOCATED` intervals that need to be freed must first be `mchain_free()`d; after freezing, release operations are no longer accepted.

### 14.4 Region Management APIs

#### `mchain_add`

```c
int mchain_add(phys_addr_t base, phys_size_t size,
               unsigned int attrs, int nid);
```

Registers a piece of available physical memory with the allocator.

- Inserts only `MCHAIN_STATE_FREE` entries; `attrs` are the attributes of this memory (e.g. `MCHAIN_ATTR_HOTPLUG`).
- Uses memblock-style "existing entries take precedence" semantics: bytes in the target range already covered by any entry remain unchanged, and only uncovered gaps are inserted as `FREE | attrs`. Therefore repeated additions and partially overlapping additions are safe.
- Newly inserted `FREE` entries merge with adjacent `FREE` entries with the same `attrs` / `nid`; this may extend an existing `FREE` entry but will not rewrite the attributes of already-covered bytes.
- `size == 0` is a successful no-op.
- `base + size` wrapping or `attrs` containing invalid bits returns `-EINVAL`.
- If new entries are needed but the static arena is full and expansion is not enabled/failed, returns `-ENOMEM`; no partial addition occurs in the target range. If expansion is triggered, expansion avoids `[base, base + size)` and will not use the memory being added as an arena; however, expansion itself may already have consumed other free memory.
- This function is not responsible for reserving the kernel image; the caller should explicitly call `mchain_reserve()` after adding all available memory.
- Returns `-EBUSY` after `mchain_freeze()`.

Example:

```c
mchain_add(0x80000000, 0x10000000, 0, 0);
mchain_add(0x90000000, 0x10000000, 0, 0);  /* The two segments are adjacent and merge into one FREE segment */
```

#### `mchain_reserve`

```c
int mchain_reserve(phys_addr_t base, phys_size_t size,
                   unsigned int attrs, int nid);
```

Converts free memory within the target range to reserved.

- Only processes bytes already registered as `FREE` by `mchain_add()`: for each free sub-interval intersecting `[base, base + size)`, carve it out and insert it as `MCHAIN_STATE_RESERVED | (source attrs | attrs)`; if the sub-interval exactly covers the entire free entry, the state can also be changed in place to avoid occupying an extra entry. Holes that have not been added will not be recorded as reserved, so all available memory must be added before reserving.
- Bytes already `RESERVED` or `ALLOCATED` remain as-is and will not be overwritten, split, or have their attributes changed. Therefore partial overlap is allowed, and the same range can be reserved repeatedly.
- Newly reserved bytes **retain the attributes of the source free entry**, and the `attrs` parameter is bitwise-ORed in. Thus attributes such as NOMAP / HOTPLUG / MIRROR / DRIVER are not lost when the state changes from `FREE` to `RESERVED`; `attrs` only adds, never removes, and clearing attributes requires an additional attribute modification API.
- If the entire target range falls in non-free regions, returns success without doing anything. Returning 0 does not mean the entire range became `RESERVED`; callers needing strict semantics should first check with `mchain_range_is_free()`.
- `size == 0` is a successful no-op.
- Invalid parameters return `-EINVAL`; insufficient entries and unable to expand return `-ENOMEM`, and no partial reservation is left in the target range. Expansion avoids the target range `[base, base + size)` and will not allocate a new arena from the memory being reserved; however, expansion itself may already have consumed other free memory.
- The target range operation of `mchain_reserve()` is atomic: there will be no intermediate state of "carved out part but reservation insertion failed".
- Returns `-EBUSY` after `mchain_freeze()`.

#### `mchain_unreserve`

```c
int mchain_unreserve(phys_addr_t base, phys_size_t size);
```

Converts reserved memory within the target range back to free.

- Only processes `RESERVED` bytes; `FREE` and `ALLOCATED` bytes remain unchanged.
- Supports partial release; even if multiple reserved areas have merged into a larger `RESERVED` entry, only one segment can be released. If the released interval exactly covers the entire reserved entry, it is directly changed in place to `FREE` without extra entries.
- Released bytes are re-inserted in the `FREE` state, retaining the `attrs` and `nid` of the original reserved entry; the `name_id` of released `FREE` bytes must be set to `MCHAIN_NAME_NONE`, as names belong only to the part still in the `RESERVED` state. They then merge with adjacent free entries.
- If the entire target range is not reserved memory, returns success without doing anything.
- `size == 0` is a successful no-op.
- Invalid parameters return `-EINVAL`; if splitting requires entries but pre-allocation fails, returns `-ENOMEM`, and no partial release is left in the target range. Expansion avoids the target range but may already have consumed other free memory as an arena.
- This function does not free `ALLOCATED` entries; freeing allocations must use `mchain_free()`.
- Returns `-EBUSY` after `mchain_freeze()`.

#### Named Reservations

```c
int mchain_reserve_named(const char *name, phys_addr_t base, phys_size_t size,
                         unsigned int attrs, int nid);
int mchain_find_reserved(const char *name, phys_addr_t *base, phys_size_t *size);
```

- `mchain_reserve_named()` is equivalent to `mchain_reserve()`, and additionally registers `name` in the static string table and writes the entry's `name_id`.
- The implementation must first look up or register the name in the name table, then perform the reservation; when the name table is full, returns `-ENOSPC`, and the region list has not yet been modified. If the subsequent reservation fails with `-ENOMEM`, an unreferenced name may remain in the name table, but no partial reservation is left.
- `name_id` participates in the merge condition (§9); adjacent reservations with different names are not merged.
- `mchain_find_reserved()` looks up the first reserved region with the same name, returns 0 on success and writes out `base` / `size`; returns `-ENOENT` if not found, and `-EINVAL` if `name == NULL`.
- This implementation already includes this group of APIs; `MCHAIN_NAME_NONE == 0` means unnamed.

### 14.5 Allocation and Freeing APIs

#### `mchain_find_in_range`

```c
phys_addr_t mchain_find_in_range(phys_size_t size, phys_size_t align,
                                 phys_addr_t min_addr, phys_addr_t max_addr,
                                 int nid, unsigned int attr_mask);
```

Only searches for free intervals satisfying the conditions; does not modify the list or trigger expansion. Returns 0 after `mchain_freeze()`, preventing callers from continuing to plan mchain allocations after handover.

Parameters:

| Parameter   | Meaning                                                                                                                                                       |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `size`      | Requested bytes, must be greater than 0                                                                                                                       |
| `align`     | Start address alignment, must be a power of 2 greater than 0; `1` means no restriction                                                                        |
| `min_addr`  | Allowed lower bound (inclusive); `MCHAIN_ANY_ADDR` means use `min_limit`                                                                                      |
| `max_addr`  | Allowed upper bound (exclusive); `MCHAIN_ANY_ADDR` means use `max_limit`                                                                                      |
| `nid`       | Node ID; `MCHAIN_NID_ANY` matches any node, and a specific node also matches `MCHAIN_NID_ANY` entries                                                         |
| `attr_mask` | Attribute bits that must match, only bits in `MCHAIN_ATTR_MASK`; `MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER` are skipped by default unless explicitly required |

The scan direction is determined by the global `mchain_set_bottom_up()`: `true` from head to tail, `false` from tail to head; default `true`.

Effective range:

```
lo = max(min_addr == MCHAIN_ANY_ADDR ? 0 : min_addr, pool.min_limit)
hi = min(max_addr == MCHAIN_ANY_ADDR ? MCHAIN_ANY_ADDR : max_addr, pool.max_limit)
```

If `lo >= hi` or `size > hi - lo`, return 0 directly.

Candidate address calculation:

- **Bottom-up**: within the free entry `[b, e)`, `t = min(e, hi)`,
  `c = align_up(max(b, lo), align)`; require that the alignment calculation does not wrap, `c >= max(b, lo)`, `c + size` does not wrap, and `c + size <= t`. If `c == 0`, try the next aligned address (usually `align`) within the same free entry; if still not satisfied, skip the entry.
- **Top-down**: `t = min(e, hi)`; require `t >= size`,
  `c = align_down(t - size, align)`; require `c >= max(b, lo)` and `c != 0`. When `c == 0`, there is no lower candidate address, so skip the entry directly.

Return the first `c` satisfying the conditions; return 0 if none. Physical address 0 is never returned as a successful result.

#### `mchain_alloc`

```c
phys_addr_t mchain_alloc(phys_size_t size, phys_size_t align,
                         phys_addr_t min_addr, phys_addr_t max_addr,
                         int nid, unsigned int attr_mask);
```

The search rules are exactly the same as `mchain_find_in_range()`; on success, the following steps are performed in order:

1. First find the candidate `candidate` and the free entry it resides in, but **do not modify the list**.
2. Compute the number of extra entries needed based on the candidate's position in the entry: 0 when covering the entire free entry; 1 when flush against the left or right end of the entry; 2 when in the middle of the entry. Do not always pre-allocate 2, otherwise when the arena has only a few slots left, expansion may be triggered unnecessarily or even fail. The candidate search must carry an entry budget: `needed` must not exceed the current `capacity - used`; candidates that exceed the budget and cannot be resolved by expansion should be skipped, continuing the search.
3. If `needed` exceeds the current number of free entries, call `mchain_ensure_entries(pool, needed, candidate, candidate + size)`. Expansion avoids the candidate interval, so it will not use the memory being allocated as an arena. If pre-allocation triggered expansion, the list structure may have changed, and step 1 must be re-executed to find the candidate and recompute `needed`; if still insufficient, repeat this step, continuing to avoid the new candidate interval. **Do not immediately return 0 when expansion fails or re-search fails**: continue searching for candidates that satisfy the current entry budget (e.g. when the arena has only 1 slot left, flush candidates are still usable), and return 0 only when no feasible candidate exists.
4. Carve the exact `[candidate, candidate + size)` from the selected free entry. If the candidate covers the entire free entry, directly change the entry to `ALLOCATED` in place; otherwise reuse the original entry to carry one fragment, and take entries from the free list only for the truly new fragments.
5. Mark the allocated interval as `MCHAIN_STATE_ALLOCATED`, **inheriting all attributes and `nid` of the source free entry**; `attr_mask` is only used for filtering and is not used to truncate attributes.
6. Return `candidate`.

Behavior conventions:

- Only the exact requested size is allocated, without absorbing left/right alignment fragments (§6.1). Therefore `mchain_free(candidate, size)` can always precisely match this allocation.
- `ALLOCATED` entries never merge with adjacent entries, even if attributes, `nid`, and addresses are contiguous.
- `mchain_find_in_range()` only performs address/attribute filtering and does not check the arena entry budget; `mchain_alloc()` additionally applies the above budget rules. When expansion is disabled and the arena has only 1 slot left, whole-segment or flush candidates should still succeed, and must not fail entirely just because the first candidate in the list is in the middle.
- `size == 0`, `align` not a power of 2, `nid < MCHAIN_NID_ANY`, or parameters causing address interval wrapping return 0.
- `attr_mask` uses only attribute bits; the implementation should AND it with `MCHAIN_ATTR_MASK`, and state bits do not participate in matching.
- The `nid` of the allocated entry inherits from the source free entry; the requested `nid` is only used for filtering and will not rewrite a piece of `MCHAIN_NID_ANY` physical memory to a specific node. If node binding is needed, a specific `nid` should be written during `mchain_add()` / `mchain_reserve()`.
- Return 0 when there is no free interval satisfying the conditions, or when entries are insufficient and expansion fails.
- Return 0 after `mchain_freeze()`.
- On failure, the target allocation leaves no partial state. If pre-allocation triggered expansion, expansion consumes a piece of free memory as the arena and registers it as `ALLOCATED`; this is a possible side effect left by the failed call, not part of the target allocation.
- Each failure increments `alloc_failures`, used for statistics.

#### `mchain_free`

```c
int mchain_free(phys_addr_t base, phys_size_t size);
```

Frees an allocation returned by `mchain_alloc()`.

- Look up an entry in the list with `state == MCHAIN_STATE_ALLOCATED` and exactly equal `base` and `size`.
- After finding it, change its state to `FREE`, retain the original attributes and `nid`, and merge with adjacent free entries with the same key.
- Partial freeing is not supported: half of an allocation cannot be freed, nor can it be freed with a different `size`. Since `ALLOCATED` entries never merge, the boundary of each allocation is always exact.
- `size == 0` returns `-EINVAL`.
- If no exactly matching `ALLOCATED` entry is found, returns `-ENOENT`; if the target range is `RESERVED`, also returns `-ENOENT`, and `mchain_unreserve()` should be used instead.
- `base + size` wrapping returns `-EINVAL`.
- Freeing does not require new entries and does not trigger expansion.
- Returns `-EBUSY` after `mchain_freeze()`.

### 14.6 Query APIs

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

| API                    | Semantics                                                                                                                                                                   |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `mchain_range_is_free` | Returns true iff every byte of `[base, base + size)` belongs to a `FREE` entry; returns true when `size == 0`; returns false on interval wrapping                           |
| `mchain_is_memory`     | Returns true iff `addr` is covered by an entry in any state; used for queries such as `pfn_valid()`                                                                         |
| `mchain_free_size`     | Sum of bytes of all `FREE` entries                                                                                                                                          |
| `mchain_reserved_size` | Sum of bytes of all `RESERVED` and `ALLOCATED` entries                                                                                                                      |
| `mchain_memory_size`   | Sum of bytes of all entries (three states)                                                                                                                                  |
| `mchain_start_of_dram` | `base` of the first entry in the list; returns 0 when the list is empty (empty list and real address 0 are indistinguishable; use `mchain_memory_size() == 0` to determine) |
| `mchain_end_of_dram`   | `base + size` of the last entry in the list; returns 0 when the list is empty                                                                                               |
| `mchain_used_entries`  | Number of entries in the current region list                                                                                                                                |
| `mchain_capacity`      | Current arena capacity                                                                                                                                                      |

When there is no overflow:

```
mchain_free_size() + mchain_reserved_size() == mchain_memory_size()
```

These functions are read-only, do not trigger expansion, and do not modify the list. Note that `mchain_range_is_free()` only checks whether the state is `FREE`, without considering attributes such as `MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER`; the iterator and allocator skip these regions by default, so the semantics differ.

### 14.7 Iterator API

```c
void mchain_iter_init(struct mchain_iter *it, phys_addr_t min_addr, phys_addr_t max_addr,
                      int nid, unsigned int attr_mask);
int mchain_iter_next(struct mchain_iter *it, phys_addr_t *start, phys_addr_t *end);
```

`mchain_iter_init()`:

- Initializes the iterator, with `cur` pointing to the head of the list.
- `min_addr == MCHAIN_ANY_ADDR` means no lower bound; `max_addr == MCHAIN_ANY_ADDR` means no upper bound.
- When `min_addr >= max_addr`, the iterator ends immediately.
- The matching rules for `nid` / `attr_mask` are the same as `mchain_alloc()`, including the rule that `MCHAIN_ATTR_NOMAP` / `MCHAIN_ATTR_DRIVER` are skipped by default.

`mchain_iter_next()`:

- When an interval is found successfully, writes `*start` / `*end` and returns 1; returns 0 when iteration is finished.
- Only returns the intersection of `FREE` entries with `[min_addr, max_addr)`; non-`FREE` entries, entries with non-matching `nid`, non-matching attributes, and no intersection are skipped.
- Each entry produces at most one interval, so no intra-entry offset field is needed.
- Output pointers must not be NULL; debug builds should assert.
- No API that modifies the list may be called during iteration; to modify, end the iteration first and re-initialize.

Typical usage:

```c
struct mchain_iter it;
phys_addr_t start, end;

mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
while (mchain_iter_next(&it, &start, &end))
        free_range_to_buddy(start, end);
```

### 14.8 Debugging and Statistics APIs

```c
int mchain_validate(void);
void mchain_dump(void);
void mchain_get_stats(struct mchain_stats *stats);
```

- `mchain_validate()`: first checks the preconditions when `free_list_ready == 0` (`head == -1`, `free_head == -1`, `used == 0`), then ensures `mchain_bootstrap(&mchain_pool_instance)` has been executed, then checks all invariants in §14.12. Returns 0 on success and `-EINVAL` on inconsistency. Complexity O(n), intended only for debug builds or late boot; does not modify the region list.
- `mchain_dump()`: first ensures `mchain_bootstrap(&mchain_pool_instance)` has been executed, then prints each entry's `base`, `size`, state name, attribute names, `nid`, `name_id`, index, and `prev` / `next` in list order, and prints the arena base address, capacity, `used`, `free_head`. Uses the kernel logging interface, without allocating memory.
- `mchain_get_stats()`: copies the current statistics to `stats`; no-op when `stats == NULL`. Field meanings:
  - `used_entries`: number of entries in the current list.
  - `peak_used_entries`: historical peak.
  - `capacity`: current arena capacity.
  - `grow_count`: number of successful expansions.
  - `alloc_failures`: cumulative number of `mchain_alloc()` failures.
  - `frozen`: whether frozen via `mchain_freeze()`.
  - `free_bytes` / `reserved_bytes` / `allocated_bytes`: byte counts of the `FREE`, `RESERVED`, and `ALLOCATED` states respectively. Adding `reserved_bytes + allocated_bytes` corresponds to `mchain_reserved_size()`.

Debug builds are recommended to execute after each modification:

```c
assert(mchain_validate() == 0);
```

### 14.9 Internal APIs

The following functions are not public and are used only by `mchain.c` and unit tests. The caller must guarantee single-threading, valid parameters, and that entry pre-allocation or locking (if locks are introduced in the future) has been completed per contract.

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

| Function                 | Contract                                                                                                                                                                                                                                                                                                                                                                             |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `mchain_bootstrap`       | If `pool->free_list_ready == 0`, string all entries in `pool->entries` into the free list and set `free_head = pool->capacity ? 0 : -1`, `free_list_ready = 1`; idempotent, does not allocate memory. Public APIs pass `&mchain_pool_instance`; unit tests can pass their own pool                                                                                                   |
| `mchain_ensure_entries`  | Ensures at least `nr` free entries; when insufficient and `can_grow == 1`, calls `mchain_grow_pool()`, and the expansion interval must not intersect `[avoid_start, avoid_end)`. Returns 0 on success; returns `-ENOMEM` on failure, at which point the target `avoid` range has not been modified, but some expansion may already have completed and consumed other free memory     |
| `mchain_insert_entry`    | Inserts a region that does not overlap any entry; the caller must have ensured free entries are available. `state` must be one of `FREE` / `RESERVED` / `ALLOCATED`, and `attrs` contain only attribute bits; only `RESERVED` may have a non-zero `name_id`; merges per §4.2; returns 0 on success and `-EINVAL` on invalid parameters                                               |
| `mchain_carve_range`     | Carves `[start, end)` from entries matching `state` / `attr_mask` / `nid`; non-matching entries remain unchanged. The caller must precisely compute and pre-allocate extra entries for each intersecting entry (at most 2). Returns 0 on success and returns the number of carved bytes via `carved`; returns `-ENOMEM` / `-EINVAL` on failure without leaving partial modifications |
| `mchain_grow_pool`       | Expands according to the bootstrap flow in §7.2, avoiding `[avoid_start, avoid_end)` when searching for a free interval; returns 0 on success and `-ENOMEM` when no suitable physical interval is found. Requires `can_grow == 1` before the call; the function temporarily sets it to 0 and must restore it to 1 on both success and failure                                        |
| `mchain_unlink_entry`    | Removes the specified entry from the region list, fixes the forward and backward links, and pushes it onto the free list; does not handle merging                                                                                                                                                                                                                                    |
| `mchain_merge_neighbors` | Attempts to merge the specified entry with the preceding and following entries per §4.2; returns the index of the merged entry. `ALLOCATED` entries directly return the original index                                                                                                                                                                                               |

`mchain_insert_entry()` already includes the `name_id` parameter (`MCHAIN_NAME_NONE` for ordinary regions), and the merge check of `mchain_merge_neighbors()` also compares `name_id`. The remaining static helper functions (such as free list push/pop, candidate address calculation, overlap check, index validity check) are implementation details and do not constitute an API.

### 14.10 Error Codes and General Conventions

| Return Value | Applicable Scenarios                                                                                                                                                                                                                      |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0`          | `int` API success; allocation-type `phys_addr_t` API failure (address 0 is the failure sentinel). For query-type APIs, 0 is a valid return value                                                                                          |
| `-EINVAL`    | Invalid parameters: `base + size` wrapping, `align` not a power of 2, `nid < MCHAIN_NID_ANY`, `attrs` / `attr_mask` containing invalid bits, invalid state value, `min_addr >= max_addr`, `size == 0` and the API does not accept a no-op |
| `-ENOMEM`    | New entries are needed but free entries are insufficient, and expansion is not enabled or expansion failed                                                                                                                                |
| `-EBUSY`     | The allocator has been frozen via `mchain_freeze()` and refuses region modification or allocation                                                                                                                                         |
| `-ENOENT`    | `mchain_free()` cannot find an exactly matching `ALLOCATED` entry; `mchain_find_reserved()` cannot find the name                                                                                                                          |
| `-ENOSPC`    | The static name table for named reservations is full                                                                                                                                                                                      |
| `1` / `0`    | `mchain_iter_next()` means "return an interval" and "iteration finished" respectively                                                                                                                                                     |

General rules:

- The global pool is statically initialized at definition; there is no `mchain_init()`. The first modification API that needs the free list calls the internal `mchain_bootstrap()`; before that, queries/iteration see an empty list.
- `mchain_find_in_range()` and `mchain_alloc()` use 0 to indicate failure and cannot distinguish "invalid parameters" from "no suitable interval". For diagnosis, combine `mchain_validate()`, `mchain_get_stats()`, and checking the call parameters.
- `mchain_alloc()` never returns physical address 0; if a piece of free memory contains only address 0 and cannot be aligned to a non-zero address, it is skipped.
- Public APIs do not sleep, block, or call slab / buddy system / vmalloc; expansion uses only `phys_to_virt()` and ordinary memory copies.
- mchain is not SMP-safe. It is called by a single core during early boot; if used in a multicore or interrupt context in the future, locking must be added on the caller side.
- APIs that modify the list invalidate all outstanding `struct mchain_iter`. After `mchain_freeze()`, the list can no longer be modified, so the final iteration can safely be completed after freezing.
- The target region operation of all modification APIs is atomic: either it takes effect completely, or no partial modification occurs in the target range. Expansion is an independent internal operation that may already have consumed a piece of free memory as an arena and registered it as `ALLOCATED`; the caller cannot assume that `mchain_free_size()` remains completely unchanged after a failure.
- Debug assertions are not part of the API contract; production builds must not rely on assertions to catch caller errors and should still return error codes.

### 14.11 Boot-Time Call Example

```c
static void __init mchain_early_setup(void)
{
        phys_addr_t start, end, p, dram_start, kernel_start, dtb_start;
        phys_size_t kernel_size, dtb_size;
        unsigned int reserved_attrs;
        struct mchain_iter it;

        /* 1. The global pool is already statically initialized; set limits and direction as needed here */
        mchain_set_limit(0, MCHAIN_ANY_ADDR);
        mchain_set_bottom_up(true);

        /* 2. First add the available memory reported by firmware */
        for_each_fdt_memory_range(start, end)
                mchain_add(start, end - start, 0, 0);
        dram_start = mchain_start_of_dram();

        /* 3. Then reserve firmware-occupied areas, the kernel image, DTB, initrd, etc.
         *    For no-map nodes, reserved_attrs should include MCHAIN_ATTR_NOMAP.
         */
        reserved_attrs = 0;
        for_each_fdt_reserved_range(start, end)
                mchain_reserve(start, end - start, reserved_attrs, 0);
        if (dram_start < kernel_start)
                mchain_reserve(dram_start, kernel_start - dram_start, 0, 0);
        mchain_reserve(kernel_start, kernel_size, 0, 0);
        mchain_reserve(dtb_start, dtb_size, 0, 0);

        /* 4. Allow arena expansion after the direct mapping is established */
        mchain_allow_grow();

        /* 5. Normal allocation; returns 0 on failure */
        p = mchain_alloc(PAGE_SIZE, PAGE_SIZE, 0, MCHAIN_ANY_ADDR,
                         MCHAIN_NID_ANY, 0);
        if (!p)
                panic("mchain: early allocation failed");

        /* 6. Freeing must use the exact size used at allocation time */
        mchain_free(p, PAGE_SIZE);

        /* 7. Freeze the allocator to prevent new allocations during handover */
        mchain_freeze();

        /* 8. Hand over to the buddy system: consume only ordinary FREE intervals */
        mchain_iter_init(&it, 0, MCHAIN_ANY_ADDR, MCHAIN_NID_ANY, 0);
        while (mchain_iter_next(&it, &start, &end))
                free_range_to_buddy(start, end);

        assert(mchain_validate() == 0);
        mchain_dump();
}
```

The global pool is already statically initialized at definition; there is no `mchain_init()`; the first `mchain_add()` establishes the free list through the internal `mchain_bootstrap()`. Note the call order: `mchain_add()` only adds available memory, and `mchain_reserve()` only carves reservations out of already-existing free memory. **All available memory must be added before performing reservations**; if reservations are performed before adding, reservation calls falling in holes will be ignored, and later-added `FREE` will not automatically become reserved. The kernel image and static arena must be reserved before the kernel starts using mchain allocations. After calling `mchain_freeze()`, mchain no longer owns any allocatable memory, and subsequent management should be fully taken over by the buddy system.

### 14.12 Invariants

`mchain_validate()` checks the following conditions:

1. `entries != NULL`, `capacity > 0`, `used <= capacity`.
2. `head == -1` iff the list is empty; `free_list_ready` is 0 or 1. If 0, then `head == -1`, `free_head == -1`, and `used == 0`; if 1, then `free_head == -1` iff there are no free entries. `mchain_validate()` checks this precondition before bootstrap, then establishes the free list, so this field should be 1 during final validation.
3. All indices of the region list are within `[0, capacity)` and there are no cycles.
4. The list is strictly ascending by `base`, entries do not overlap, and `base + size` does not wrap.
5. For each entry, `prev` / `next` are inverses of each other: when `entries[i].next != -1`, `entries[entries[i].next].prev == i`, and vice versa; the list head has `prev == -1` and the tail has `next == -1`.
6. The list length equals `used`.
7. `flags & MCHAIN_STATE_MASK` of each list entry is exactly one of `FREE` / `RESERVED` / `ALLOCATED`; `flags & ~(MCHAIN_STATE_MASK | MCHAIN_ATTR_MASK)` is 0 (reserved bits unused); `nid >= MCHAIN_NID_ANY`; only `RESERVED` entries may have a non-zero `name_id`, and `FREE` / `ALLOCATED` entries must have `name_id == MCHAIN_NAME_NONE`.
8. No two adjacent entries satisfy the merge condition in §4.2 (i.e. there is no `FREE` / `RESERVED` pair that should have been merged but was not; `ALLOCATED` is excluded by the merge condition).
9. When `free_list_ready == 1`, all indices in the free list are within `[0, capacity)`, with no duplicates, no cycles, and disjoint from the region list; the free list length equals `capacity - used`.
10. `peak_used >= used`; `grow_count` / `alloc_failures` only increase.
11. `mchain_free_size() + mchain_reserved_size() == mchain_memory_size()` (assuming no overflow).
12. `can_grow`, `frozen`, and `free_list_ready` can only be 0 or 1; during expansion `can_grow == 0`; when `frozen == 1`, `can_grow == 0`.
13. `bottom_up` can only be `true` or `false`; it only affects subsequent lookups/allocations and does not affect existing entries.

Any modification API should satisfy these invariants before returning in debug builds. Production builds can compile out the `mchain_validate()` calls but must not change the API semantics.
