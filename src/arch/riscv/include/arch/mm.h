#ifndef ARCH_MM_H
#define ARCH_MM_H

#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/pgtable_types.h>
#include <brk/asm/cpu.h>
#include <brk/kernel.h>
#include <brk/mm_types.h>
#include <brk/types.h>

extern struct kernel_mapping {
	size_t kernel_va_pa_offset;
} kernel_map;

extern u64 phys_ram_base;

extern char _skernel[], _ekernel[];
extern char _stext[], _etext[];
extern char _srodata[], _erodata[];
extern char _sdata[], _edata[];
extern char _sbss[], _ebss[];
extern char _secondary_start[];

#define NR_EARLY_PGDIR_PAGES 8
#define NR_RSV_PAGES (NR_EARLY_PGDIR_PAGES + MAX_CPUS * INIT_STACK_SIZE)

/*
 * All page tables below the top-level PGD are carved out of the early_pgdir
 * region. The frontier must be global: the identity, link and DTB mappings
 * are built by separate map_range() calls that must not reuse each other's
 * tables.
 */
extern u64 rsv_page_next;
extern u8 rsv_pages[PAGE_SIZE * NR_RSV_PAGES] __attribute__((aligned(PAGE_SIZE)));

static inline u64 symbol_phys(void *vaddr)
{
	return (uintptr_t)vaddr - kernel_map.kernel_va_pa_offset;
}

#define _SKERNEL_PHYS symbol_phys(_skernel)
#define _EKERNEL_PHYS symbol_phys(_ekernel)
#define _KERNEL_SIZE ((uintptr_t)_ekernel - (uintptr_t)_skernel)

#define _STEXT_PHYS symbol_phys(_stext)
#define _ETEXT_PHYS symbol_phys(_etext)
#define _ETEXT_ALIGNED_PHYS round_up(_ETEXT_PHYS, PAGE_SIZE)
#define _TEXT_SIZE ((uintptr_t)_etext - (uintptr_t)_stext)

#define _SRODATA_PHYS symbol_phys(_srodata)
#define _ERODATA_PHYS symbol_phys(_erodata)
#define _ERODATA_ALIGNED_PHYS round_up(_ERODATA_PHYS, PAGE_SIZE)
#define _RODATA_SIZE ((uintptr_t)_erodata - (uintptr_t)_srodata)

#define _SDATA_PHYS symbol_phys(_sdata)
#define _EDATA_PHYS symbol_phys(_edata)
#define _EDATA_ALIGNED_PHYS round_up(_EDATA_PHYS, PAGE_SIZE)
#define _DATA_SIZE ((uintptr_t)_edata - (uintptr_t)_sdata)

#define _SBSS_PHYS symbol_phys(_sbss)
#define _EBSS_PHYS symbol_phys(_ebss)
#define _EBSS_ALIGNED_PHYS round_up(_EBSS_PHYS, PAGE_SIZE)
#define _BSS_SIZE ((uintptr_t)_ebss - (uintptr_t)_sbss)

static inline uintptr_t phys_to_virt(u64 paddr)
{
	return (paddr - phys_ram_base) + PAGE_OFFSET;
}

static inline u64 virt_to_phys(uintptr_t vaddr)
{
	return (vaddr - PAGE_OFFSET) + phys_ram_base;
}

void paging_init(void);
void final_pgtable_enable(void);

#endif
